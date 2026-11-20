# Field queue dan uji multi-hop

Dokumen ini membedakan dua antrean yang sering terlihat sebagai masalah yang sama:

- `pending_sos` / `pending_locations` adalah outbox aplikasi Field Node. SOS tetap disimpan sampai ACK `STORED` dari backend kembali melalui Gateway.
- `tx_queue` adalah antrean internal LoRaMesher. Firmware memberi backpressure pada kedalaman 8 dari kapasitas 10 dan hanya mengizinkan dua frame mobile yang belum dikonfirmasi berada dalam window kirim.

Tanpa Gateway, atau bila jalur Gateway -> serial bridge -> MQTT -> backend -> ACK terputus, outbox tidak dapat dikuras dengan aman. Firmware tidak menghapus SOS yang belum dikonfirmasi. Satu ACK yang hilang tidak lagi menahan semua item berikutnya: scheduler dapat mengirim dua key berbeda, menerima ACK tidak berurutan, lalu membuka slot untuk item berikutnya.

## Membaca status

`GET /api/status` kini memuat:

- `delivery_state`: `idle`, `mesh_not_started`, `mesh_not_synchronized`, `gateway_not_found`, atau `sending_or_awaiting_server_ack`;
- `queue_full` dan `sos_free_slots`;
- `gateway_address`, `gateway_next_hop`, `gateway_hops`, dan `gateway_link_quality`.

Rute langsung memiliki `gateway_hops=1` dan biasanya `gateway_next_hop=gateway_address`. Rute relay memiliki `gateway_hops>1` dan `gateway_next_hop` berisi alamat mesh Field Node yang menjadi jembatan. `NODE_ID` aplikasi harus unik pada setiap Field Node, tetapi alamat mesh berasal dari hardware dan dicetak oleh log `[MESH] started address=...`.

LoRaMesher memilih rute distance-vector dengan biaya yang menggabungkan hop count dan kualitas link. Aplikasi selalu mengirim ke `gateway_address`; library meneruskan frame ke `gateway_next_hop`, dan relay berikutnya melakukan hal yang sama. Aplikasi tidak melakukan flooding manual kedua karena hal itu akan menduplikasi paket dan memenuhi airtime.

## Compile dan tanam firmware

Semua perangkat harus memakai versi firmware/LoRaMesher dan konfigurasi radio yang sama. Pasang antena sebelum board diberi daya. Jangan memakai `erase-flash` atau mengaktifkan **Erase All Flash** karena Field Node dapat memiliki SOS tertunda di partisi `rn_sos`.

Di setiap Field Node, ubah `NODE_ID` pada `field_node/field_node.ino` menjadi nilai unik 1-15. Buat juga `field_node/tls_secrets.h` dari sertifikat lokal yang valid mengikuti `RESCUENET_LOCAL_HTTPS_SETUP.md`. Kemudian buka PowerShell dari root repo:

```powershell
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:ttgo-lora32:Revision=TTGO_LoRa32_V1,EraseFlash=none'
& $cli board list
& $cli compile --jobs 1 --fqbn $fqbn --build-property 'upload.maximum_size=1703936' --build-path (Join-Path $PWD '.build-gateway-multihop') (Join-Path $PWD 'gateway_node')
& $cli compile --jobs 1 --fqbn $fqbn --build-property 'upload.maximum_size=1703936' --build-path (Join-Path $PWD '.build-field-multihop') (Join-Path $PWD 'field_node')
```

Setelah kedua compile lulus, ganti placeholder port berikut dengan port hasil `board list`. Upload Gateway lebih dahulu, lalu upload satu Field pada satu waktu. Compile ulang ke build path berbeda setiap kali `NODE_ID` diubah:

```powershell
& $cli upload --fqbn $fqbn --port COM_GATEWAY --input-dir (Join-Path $PWD '.build-gateway-multihop') (Join-Path $PWD 'gateway_node')
& $cli upload --fqbn $fqbn --port COM_FIELD --input-dir (Join-Path $PWD '.build-field-multihop') (Join-Path $PWD 'field_node')
```

Upload biasanya hanya beberapa menit. Compile pertama dapat jauh lebih lama karena seluruh core ESP32 dan library dibangun. Setelah upload, buka Serial Monitor 115200 baud dan ikuti urutan nyala pada bagian berikut.

## Uji fisik dua hop

1. Flash versi firmware dan LoRaMesher yang sama ke Gateway, Relay A, dan Edge B. Gunakan `NODE_ID` berbeda untuk A dan B serta parameter frekuensi/SF/BW/CR/sync word yang sama.
2. Nyalakan Gateway lebih dulu dan tunggu `GATEWAY_READY`.
3. Nyalakan Relay A dalam jangkauan Gateway. Tunggu status A menunjukkan sinkron dan `gateway_hops=1`.
4. Tempatkan Edge B sehingga dapat menjangkau A tetapi tidak dapat menjangkau Gateway. Pengujian ini harus dibuktikan secara fisik; tiga perangkat yang diletakkan berdekatan hampir selalu memilih rute langsung.
5. Nyalakan B dan tunggu beberapa superframe. Syarat lulus discovery B: `mesh_synchronized=true`, `gateway_found=true`, `gateway_hops=2`, dan `gateway_next_hop` sama dengan alamat mesh A.
6. Kirim satu SOS baru dari B. Log B harus mencetak `[MOBILE TX] ... next_hop=<A> hops=2`. Gateway harus menerima source asli B, backend menyimpan key yang sama, lalu B menerima `[SERVER ACK]` dan `pending_sos` berkurang.
7. Matikan atau jauhkan A. B harus kehilangan rute lama; jika ada relay alternatif yang valid, `gateway_next_hop` berubah. Jika tidak ada jalur lain, status yang benar adalah `gateway_found=false` dan SOS tetap tersimpan, bukan dianggap terkirim.

Jangan menilai multi-hop hanya dari `route_count`. Bukti minimal adalah pasangan `gateway_hops`, `gateway_next_hop`, key yang sama pada Gateway/backend, dan ACK yang mengurangi pending di node asal.
