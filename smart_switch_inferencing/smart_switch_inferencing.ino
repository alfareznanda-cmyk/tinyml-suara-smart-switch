#include <proyek_Tinyml_suara_inferencing.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s.h"
#include <math.h>
#include <string.h>

/** Audio buffers, pointers and selectors */
typedef struct {
    int16_t *buffer;
    uint8_t buf_ready;
    uint32_t buf_count;
    uint32_t n_samples;
} inference_t;

static inference_t inference;
static const uint32_t sample_buffer_size = 2048;
static signed short sampleBuffer[sample_buffer_size];
static bool debug_nn = true;
static bool record_status = true;

// Menghitung energi sinyal (Root Mean Square) dari satu window audio.
// Dipakai sebagai gerbang silence-detection sebelum window dikirim ke classifier.
// Kompleksitas: O(n), n = jumlah sampel dalam window.
float computeRMSEnergy(const int16_t* buf, uint32_t len) {
    double sumSquares = 0;
    for (uint32_t i = 0; i < len; i++) {
        sumSquares += (double)buf[i] * (double)buf[i];
    }
    return sqrt(sumSquares / (double)len);
}

// Menghitung jumlah pergantian tanda (zero-crossing) dalam satu window audio.
// ZCR tinggi cenderung menandakan suara konsonan/desis, ZCR rendah menandakan
// suara vokal/hening. Kompleksitas: O(n), n = jumlah sampel dalam window.
int computeZeroCrossingRate(const int16_t* buf, uint32_t len) {
    int crossings = 0;
    for (uint32_t i = 1; i < len; i++) {
        bool prevNeg = buf[i - 1] < 0;
        bool currNeg = buf[i] < 0;
        if (prevNeg != currNeg) {
            crossings++;
        }
    }
    return crossings;
}

// Ambang energi minimum supaya window dianggap "ada suara" (bukan hening/noise).
// Kalibrasi nilai ini berdasarkan pengujian nyata di lingkungan masing-masing
// (cetak nilai energy ke Serial saat diam vs saat bicara, ambil titik tengahnya).
static float ambang_energi = 200.0;

// Buffer melingkar (circular buffer) untuk menyimpan N hasil klasifikasi
// terakhir, lalu menentukan label paling sering muncul (majority voting)
// sebelum aktuator benar-benar dieksekusi. Tujuannya meredam noise/flicker
// akibat satu prediksi yang salah.
#define VOTE_BUFFER_SIZE 5     // jumlah prediksi terakhir yang disimpan
#define LABEL_MAXLEN      16   // panjang maksimum string label

typedef struct {
    char labels[VOTE_BUFFER_SIZE][LABEL_MAXLEN];
    uint8_t head;   // posisi penulisan berikutnya (bergerak melingkar / wrap-around)
    uint8_t count;  // jumlah slot yang sudah pernah terisi (maks VOTE_BUFFER_SIZE)
} VoteBuffer;

static VoteBuffer voteBuf = { .head = 0, .count = 0 };

// Memasukkan label baru ke buffer melingkar.
// Kompleksitas: O(1).
void voteBuffer_push(const char* label) {
    strncpy(voteBuf.labels[voteBuf.head], label, LABEL_MAXLEN - 1);
    voteBuf.labels[voteBuf.head][LABEL_MAXLEN - 1] = '\0';
    voteBuf.head = (voteBuf.head + 1) % VOTE_BUFFER_SIZE;
    if (voteBuf.count < VOTE_BUFFER_SIZE) voteBuf.count++;
}

// Mencari label dengan frekuensi tertinggi di dalam buffer (voting mayoritas).
// Mengembalikan true jika ada label dominan (rasio >= minRatio) DAN buffer
// sudah terisi penuh (menghindari keputusan dari data yang belum cukup).
// Kompleksitas: O(k^2) dengan k = VOTE_BUFFER_SIZE (konstanta kecil, mis. 5).
bool voteBuffer_majority(char* outLabel, float minRatio) {
    if (voteBuf.count < VOTE_BUFFER_SIZE) return false;

    char uniqueLabels[VOTE_BUFFER_SIZE][LABEL_MAXLEN];
    int freq[VOTE_BUFFER_SIZE] = {0};
    int numUnique = 0;

    for (int i = 0; i < voteBuf.count; i++) {
        bool found = false;
        for (int j = 0; j < numUnique; j++) {
            if (strcmp(uniqueLabels[j], voteBuf.labels[i]) == 0) {
                freq[j]++;
                found = true;
                break;
            }
        }
        if (!found) {
            strcpy(uniqueLabels[numUnique], voteBuf.labels[i]);
            freq[numUnique] = 1;
            numUnique++;
        }
    }

    int maxIdx = 0;
    for (int j = 1; j < numUnique; j++) {
        if (freq[j] > freq[maxIdx]) maxIdx = j;
    }

    float ratio = (float)freq[maxIdx] / (float)voteBuf.count;
    if (ratio >= minRatio) {
        strncpy(outLabel, uniqueLabels[maxIdx], LABEL_MAXLEN - 1);
        outLabel[LABEL_MAXLEN - 1] = '\0';
        return true;
    }
    return false;
}

// Deklarasi forward (forward declaration)
static bool microphone_inference_start(uint32_t n_samples);
static bool microphone_inference_record(void);
static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr);
static int i2s_init(uint32_t sampling_rate);
static int i2s_deinit(void);

/**
 * @brief      Arduino setup function
 */
void setup()
{
    Serial.begin(115200);
    while (!Serial);
    Serial.println("Edge Impulse Inferencing Demo");

    ei_printf("Inferencing settings:\n");
    ei_printf("\tInterval: ");
    ei_printf_float((float)EI_CLASSIFIER_INTERVAL_MS);
    ei_printf(" ms.\n");
    ei_printf("\tFrame size: %d\n", EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    ei_printf("\tSample length: %d ms.\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT / 16);
    ei_printf("\tNo. of classes: %d\n", sizeof(ei_classifier_inferencing_categories) / sizeof(ei_classifier_inferencing_categories[0]));

    ei_printf("\nStarting continious inference in 2 seconds...\n");
    ei_sleep(2000);

    if (microphone_inference_start(EI_CLASSIFIER_RAW_SAMPLE_COUNT) == false) {
        ei_printf("ERR: Could not allocate audio buffer (size %d), this could be due to the window length of your model\r\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT);
        return;
    }

    ei_printf("Recording...\n");

    // Inisialisasi pin lampu LED langsung (tanpa relay) di GPIO 2
    // LED langsung: HIGH = NYALA (arus mengalir GPIO -> resistor -> LED -> GND), LOW = MATI
    pinMode(2, OUTPUT);
    digitalWrite(2, LOW); // Kondisi awal lampu MATI
}

/**
 * @brief      Arduino main function. Runs the inferencing loop.
 */
void loop()
{
    bool m = microphone_inference_record();
    if (!m) {
        ei_printf("ERR: Failed to record audio...\n");
        return;
    }

    // Hitung fitur energi & ZCR dari window audio yang baru direkam,
    // sebagai gerbang silence-detection sebelum inferensi dijalankan.
    float energy = computeRMSEnergy(inference.buffer, inference.n_samples);
    int zcr = computeZeroCrossingRate(inference.buffer, inference.n_samples);
    ei_printf("Energy: %.2f | ZCR: %d\n", energy, zcr);

    if (energy < ambang_energi) {
        // Window dianggap hening / noise lemah -> lewati inferensi.
        // Menghemat komputasi DSP+NN dan mencegah false trigger saat sepi.
        ei_printf("Silence terdeteksi, skip inferensi.\n");
        return;
    }

    signal_t signal;
    signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    signal.get_data = &microphone_audio_signal_get_data;
    ei_impulse_result_t result = { 0 };

    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, debug_nn);
    if (r != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", r);
        return;
    }

    // Menampilkan hasil prediksi ke Serial Monitor
    ei_printf("Predictions ");
    ei_printf("(DSP: %d ms., Classification: %d ms., Anomaly: %d ms.)",
        result.timing.dsp, result.timing.classification, result.timing.anomaly);
    ei_printf(": \n");
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        ei_printf("    %s: ", result.classification[ix].label);
        ei_printf_float(result.classification[ix].value);
        ei_printf("\n");
    }

    // Cari label dengan probabilitas tertinggi
    int bestIdx = 0;
    for (size_t ix = 1; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        if (result.classification[ix].value > result.classification[bestIdx].value) {
            bestIdx = ix;
        }
    }

    float ambang_batas = 0.55; // Menggunakan batas peka 55%

    const char* bestLabel = result.classification[bestIdx].label;
    // Hanya label "Nyala"/"Mati" yang boleh ikut voting. Label lain (silence/unknown,
    // dst.) diabaikan sepenuhnya supaya tidak "mengencerkan" buffer saat ada jeda/napas
    // di antara ucapan kata kunci yang berulang.
    bool isActionableLabel = (strcmp(bestLabel, "Nyala") == 0) || (strcmp(bestLabel, "Mati") == 0);

    if (isActionableLabel && result.classification[bestIdx].value > ambang_batas) {
        // Masukkan label pemenang ke circular buffer, belum dieksekusi langsung.
        voteBuffer_push(bestLabel);
    }

    // Eksekusi aktuator hanya jika hasil voting sudah stabil
    char stableLabel[LABEL_MAXLEN];
    if (voteBuffer_majority(stableLabel, 0.6)) {
        if (strcmp(stableLabel, "Nyala") == 0) {
            ei_printf(">>> SAKELAR AKTIF (voting stabil): LAMPU NYALA <<<\n");
            digitalWrite(2, HIGH);  // LED langsung: HIGH = menyalakan lampu
        }
        else if (strcmp(stableLabel, "Mati") == 0) {
            ei_printf(">>> SAKELAR AKTIF (voting stabil): LAMPU MATI <<<\n");
            digitalWrite(2, LOW); // LED langsung: LOW = mematikan lampu
        }
    }

#if EI_CLASSIFIER_HAS_ANOMALY == 1
    ei_printf("    anomaly score: ");
    ei_printf_float(result.anomaly);
    ei_printf("\n");
#endif
}

static void audio_inference_callback(uint32_t n_bytes)
{
    for(int i = 0; i < n_bytes>>1; i++) {
        inference.buffer[inference.buf_count++] = sampleBuffer[i];

        if(inference.buf_count >= inference.n_samples) {
          inference.buf_count = 0;
          inference.buf_ready = 1;
        }
    }
}

static void capture_samples(void* arg) {
  const int32_t i2s_bytes_to_read = (uint32_t)arg;
  size_t bytes_read = i2s_bytes_to_read;

  while (record_status) {
    /* Membaca data suara dari port I2S */
    i2s_read((i2s_port_t)1, (void*)sampleBuffer, i2s_bytes_to_read, &bytes_read, 100);

    if (bytes_read <= 0) {
      ei_printf("Error in I2S read : %d", bytes_read);
    }
    else {
        if (bytes_read < i2s_bytes_to_read) {
            ei_printf("Partial I2S read");
        }

        // Penguatan (gain) sinyal mikrofon I2S yang amplitudonya lemah,
        // tiap sampel dikalikan 16x agar cukup kuat untuk diproses classifier.
        for (int x = 0; x < i2s_bytes_to_read / 2; x++) {
            sampleBuffer[x] = (int16_t)(sampleBuffer[x]) * 16;
        }

        if (record_status) {
            audio_inference_callback(i2s_bytes_to_read);
        }
        else {
            break;
        }
    }
  }
  vTaskDelete(NULL);
}

static bool microphone_inference_start(uint32_t n_samples)
{
    inference.buffer = (int16_t *)malloc(n_samples * sizeof(int16_t));

    if(inference.buffer == NULL) {
        return false;
    }

    inference.buf_count  = 0;
    inference.n_samples  = n_samples;
    inference.buf_ready  = 0;

    if (i2s_init(EI_CLASSIFIER_FREQUENCY)) {
        ei_printf("Failed to start I2S!");
    }

    ei_sleep(100);
    record_status = true;

    xTaskCreate(capture_samples, "CaptureSamples", 1024 * 32, (void*)sample_buffer_size, 10, NULL);
    return true;
}

static bool microphone_inference_record(void)
{
    bool ret = true;
    while (inference.buf_ready == 0) {
        delay(10);
    }
    inference.buf_ready = 0;
    return ret;
}

static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr)
{
    numpy::int16_to_float(&inference.buffer[offset], out_ptr, length);
    return 0;
}

static void microphone_inference_end(void)
{
    i2s_deinit();
    ei_free(inference.buffer);
}

static int i2s_init(uint32_t sampling_rate) {
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = sampling_rate,
      .bits_per_sample = (i2s_bits_per_sample_t)16,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = 0,
      .dma_buf_count = 8,
      .dma_buf_len = 512,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = -1,
  };

  // Konfigurasi pin I2S sesuai kabel fisik
  i2s_pin_config_t pin_config = {
      .bck_io_num = 32,    // KABEL PUTIH di D32 (SCK)
      .ws_io_num = 25,     // KABEL KUNING di D25 (WS)
      .data_out_num = -1,
      .data_in_num = 33,   // KABEL HIJAU di D33 (SD)
  };
  esp_err_t ret = 0;

  ret = i2s_driver_install((i2s_port_t)1, &i2s_config, 0, NULL);
  if (ret != ESP_OK) {
    ei_printf("Error in i2s_driver_install");
  }

  ret = i2s_set_pin((i2s_port_t)1, &pin_config);
  if (ret != ESP_OK) {
    ei_printf("Error in i2s_set_pin");
  }

  ret = i2s_zero_dma_buffer((i2s_port_t)1);
  if (ret != ESP_OK) {
    ei_printf("Error in initializing dma buffer with 0");
  }

  return int(ret);
}

static int i2s_deinit(void) {
    i2s_driver_uninstall((i2s_port_t)1);
    return 0;
}
