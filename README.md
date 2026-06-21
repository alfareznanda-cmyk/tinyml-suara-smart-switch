# TinyML Suara - Smart Switch (ESP32 + Edge Impulse)

Proyek ini menggunakan **TinyML (Edge Impulse)** pada **ESP32** untuk mengenali perintah suara ("Nyala" / "Mati") dari mikrofon I2S, lalu menyalakan/mematikan LED sebagai simulasi saklar lampu pintar (voice-controlled smart switch).

Selain inferensi suara dasar, proyek ini menambahkan dua lapisan keandalan:
- **Gerbang energi (RMS Energy) + Zero-Crossing Rate (ZCR)** untuk menyaring window hening/noise sebelum masuk ke classifier, sehingga menghemat komputasi dan mengurangi false trigger.
- **Majority voting** lewat circular buffer 5 prediksi terakhir, supaya aktuator (LED) hanya bereaksi setelah hasil klasifikasi stabil, bukan dari satu prediksi tunggal yang mungkin salah.

## Fitur

- Inferensi suara real-time/continuous menggunakan model hasil export Edge Impulse.
- Silence detection berbasis RMS Energy sebelum menjalankan classifier.
- Penguatan (gain) sinyal mikrofon I2S 16x karena amplitudo mentah biasanya lemah.
- Majority voting untuk menstabilkan keputusan aktuator dan meredam flicker.
- Kontrol LED langsung di GPIO 2 (bisa diadaptasi ke relay untuk lampu sungguhan).

## Hardware yang Dibutuhkan

| Komponen | Keterangan |
|---|---|
| ESP32 Dev Board | Mendukung I2S |
| Mikrofon I2S (mis. INMP441) | Untuk input suara |
| LED + resistor (atau modul relay) | Sebagai aktuator output |
| Kabel jumper, breadboard | Untuk wiring |

### Wiring Mikrofon I2S (INMP441) ke ESP32

| Pin Mikrofon | Pin ESP32 | Keterangan |
|---|---|---|
| SCK (BCK) | GPIO 32 | Kabel **putih** |
| WS | GPIO 25 | Kabel **kuning** |
| SD | GPIO 33 | Kabel **hijau** |
| VDD | 3.3V | |
| GND | GND | |
| L/R | GND (channel kiri) | |

### Wiring LED

| LED | Pin ESP32 |
|---|---|
| Anoda (+) lewat resistor | GPIO 2 |
| Katoda (-) | GND |

> LED langsung (tanpa relay): `HIGH` = menyalakan LED, `LOW` = mematikan LED. Jika ingin mengendalikan lampu sungguhan, ganti LED dengan modul relay dan sesuaikan logika HIGH/LOW sesuai jenis relay (active-high/active-low).

## Software yang Dibutuhkan

1. **Arduino IDE** (versi 1.8.x atau 2.x) — [unduh di sini](https://www.arduino.cc/en/software)
2. **ESP32 Board Support Package** terpasang di Arduino IDE
   (Boards Manager → cari `esp32` oleh Espressif Systems)
3. **Library hasil export Edge Impulse**, yaitu library bernama
   `proyek_Tinyml_suara_inferencing` (file `.zip` hasil export dari Edge Impulse Studio: *Deployment → Arduino library*).

## Instalasi & Setup

1. **Clone repositori ini**
   ```bash
   git clone <url-repositori-ini>
   cd tinyml-suara-smart-switch
   ```

2. **Pasang library Edge Impulse**
   - Di Edge Impulse Studio, buka project suara kamu → tab **Deployment** → pilih **Arduino library** → klik **Build**.
   - Hasilnya berupa file `.zip` (mis. `proyek_Tinyml_suara_inferencing.zip`).
   - Di Arduino IDE: **Sketch → Include Library → Add .ZIP Library...** lalu pilih file tersebut.

3. **Pasang board ESP32 di Arduino IDE** (jika belum)
   - **File → Preferences** → tambahkan URL berikut di *Additional Board Manager URLs*:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - **Tools → Board → Boards Manager** → cari `esp32` → install.

4. **Buka sketch**
   - Buka file `smart_switch_inferencing/smart_switch_inferencing.ino` di Arduino IDE.

5. **Pilih board & port**
   - **Tools → Board** → pilih board ESP32 yang sesuai (mis. `ESP32 Dev Module`).
   - **Tools → Port** → pilih port serial ESP32 kamu.

## Cara Menjalankan

1. Sambungkan ESP32 ke komputer via USB.
2. Pastikan wiring mikrofon I2S dan LED sudah sesuai tabel di atas.
3. Klik **Upload** di Arduino IDE.
4. Setelah upload selesai, buka **Serial Monitor** (baud rate `115200`).
5. Sketch akan menunggu 2 detik lalu mulai merekam dan melakukan inferensi terus-menerus.
6. Ucapkan kata kunci ("Nyala" / "Mati") di dekat mikrofon. Di Serial Monitor kamu akan melihat:
   - Nilai `Energy` dan `ZCR` tiap window.
   - Hasil prediksi tiap label beserta confidence score.
   - Status voting; LED hanya akan menyala/mati setelah hasil voting stabil (≥60% dari 5 prediksi terakhir adalah label yang sama).

## Kalibrasi

- **Ambang energi (`ambang_energi`)**: nilai awal `200.0`. Untuk kalibrasi sesuai lingkungan kamu, amati nilai `Energy` di Serial Monitor saat diam vs saat bicara, lalu pilih nilai tengah di antara keduanya sebagai ambang baru.
- **Ambang confidence (`ambang_batas`)**: nilai awal `0.55` (55%). Naikkan jika sering false positive, turunkan jika model sering tidak terdeteksi padahal sudah jelas mengucapkan kata kunci.
- **Ukuran buffer voting (`VOTE_BUFFER_SIZE`)**: nilai awal `5`. Buffer lebih besar = lebih stabil tapi responsnya lebih lambat.

## Struktur Folder

```
tinyml-suara-smart-switch/
├── README.md
├── .gitignore
└── smart_switch_inferencing/
    └── smart_switch_inferencing.ino
```

> Catatan: library hasil export Edge Impulse (`proyek_Tinyml_suara_inferencing`) **tidak** disertakan di repositori ini karena ukurannya besar dan spesifik untuk tiap model. Silakan export ulang dari Edge Impulse Studio sesuai langkah di atas.

## Troubleshooting

- **`ERR: Could not allocate audio buffer`**: ukuran window model terlalu besar untuk RAM ESP32 yang tersedia. Coba kurangi window size di Edge Impulse Studio atau gunakan board ESP32 dengan PSRAM.
- **Tidak ada suara terdeteksi / Energy selalu rendah**: cek wiring mikrofon I2S, terutama pin SCK/WS/SD, dan pastikan mikrofon mendapat daya 3.3V.
- **LED tidak merespons walau prediksi benar**: ingat sistem menunggu voting stabil (minimal 5 prediksi berturut-turut, mayoritas sama). Coba ucapkan kata kunci beberapa kali berturut-turut.

## Lisensi

Belum ditentukan — tambahkan file `LICENSE` sesuai kebutuhan (mis. MIT) jika proyek ini akan dipublikasikan.
