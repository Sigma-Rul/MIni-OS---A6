# 6S (Operating Six) - Skenario Pengujian

Setelah boot muncul logo "6S" dan prompt `root@6s#`. Ketik `skenario` untuk melihat peta ringkas di dalam OS.
Setiap demo otomatis membersihkan layar dulu, jadi hasilnya muat satu layar. Ambil screenshot SETELAH tiap
skenario selesai (untuk Bab III laporan).

Alur umum kernel: Timer (1000x/detik) -> interupsi -> `sched_tick()` -> jika quantum habis -> `schedule()` memilih
thread READY berikutnya -> `boot.S` menukar stack (context switch) -> `iret` melanjutkan thread baru.

Daftar perintah: ketik `bantuan`.
Pengguna: OS mulai sebagai root. `masuk guest` = pindah ke user biasa, `masuk root` (password 6s) = kembali.

---------------------------------------------------------------------
## 1. Bootstrapping & CLI
Perintah: `bantuan`, `tentang`, `waktu`   (juga `skenario`, `proses`, dll. = lebih dari 3 perintah buatan sendiri)
Yang terlihat: logo ASCII, baris [ OK ] tiap komponen, prompt, lalu hasil tiap perintah.
Alur: QEMU memuat 6s.elf ke RAM (multiboot) -> `_start` di boot.S -> `kmain()` menyalakan GDT, IDT, PIC, heap,
scheduler, RAMDisk, timer -> `shell_main()` membaca keyboard.
Lulus bila: OS boot di QEMU dan >= 3 perintah berjalan.

## 2. Proses & Thread (status)
Perintah: `thread`
Yang terlihat: SATU tabel ringkas, tiap baris = status shell, prime-calc, sleeper:
 1. setelah create : shell RUNNING | prime-calc NEW        | sleeper NEW
 2. setelah start  : shell RUNNING | prime-calc READY      | sleeper READY
 3. sedang berjalan: shell READY   | prime-calc RUNNING    | sleeper WAITING (tidur)
 4. selesai        : shell RUNNING | prime-calc TERMINATED | sleeper TERMINATED
Catatan: CPU hanya satu, jadi hanya 1 thread RUNNING pada satu waktu. Baris 3 dicatat oleh prime-calc sendiri,
makanya ia terlihat RUNNING.
Alur: `thread_create` membuat TCB + stack (NEW) -> `thread_start` (READY) -> scheduler memilih (RUNNING) ->
`thread_sleep` (WAITING) -> timer membangunkan (READY) -> fungsi selesai -> `thread_exit` (TERMINATED).
Lulus bila: 2 thread dengan tugas berbeda dan kelima status tampil.

## 3. Penjadwalan Round Robin (quantum bisa diubah, bisa berbeda tiap thread)
Perintah:
    jadwal               # quantum bawaan berbeda: A=10 ms, B=30 ms, C=60 ms
    jadwal 50            # semua thread quantum sama (Round Robin klasik)
    jadwal 20 20 80      # bebas: quantum A, B, C
    quantum 20           # mengubah quantum bawaan (global) untuk thread lain
Yang terlihat: 3 thread (A,B,C) mencetak huruf tiap 10 ms CPU.
 - jadwal (10/30/60): tiap putaran muncul  A  BBB  CCCCCC  (1 : 3 : 6 huruf). Porsi CPU kira-kira 10% : 30% : 60%.
 - jadwal 50        : blok huruf sama panjang  AAAAABBBBBCCCCC...
 - jadwal 5         : bergantian rapat  ABCABCABC...  dan "context switch" jauh lebih banyak.
Alur: tiap thread loop tanpa yield(); hanya timer yang memaksa pindah. Setiap thread punya field `quantum` di TCB;
`schedule()` memberi jatah waktu sesuai quantum thread yang akan jalan. Jika 0, dipakai quantum global.
Lulus bila: pola huruf bergantian (bukan printf simulasi) dan berubah mengikuti quantum.

## 4. Sinkronisasi & Race Condition
Perintah: `race`
Setelah hasil tanpa mutex tampil, OS bertanya "Lanjut dengan sinkronisasi (mutex)? (y/n)". Ketik `y` lalu Enter
untuk menjalankan versi dengan mutex. (Bisa juga langsung: `race sinkron`.)
Yang terlihat:
 - Tanpa mutex  : hasil < 200000 (ada "hilang" > 0)  => RACE CONDITION  (angka berbeda tiap run)
 - Dengan mutex : hasil = 200000, hilang = 0           => KONSISTEN
Alur: 2 thread melakukan baca-tmp-tulis(tmp+1) pada 1 counter. Tanpa mutex, thread bisa terputus timer di
antara baca dan tulis sehingga update tertimpa. Dengan mutex, bagian itu (critical section) hanya dimasuki 1 thread.

## 5. Deadlock
Perintah: `deadlock`
Setelah demo selesai, OS bertanya "Lanjut ke recovery (pulihkan)? (y/n)". Ketik `y` lalu Enter. Setelah recovery,
OS bertanya lagi "Lanjut ke pencegahan (fix)? (y/n)". Ketik `y` lagi. Jadi cukup: `deadlock`, `y`, `y`.
(Perintah terpisah tetap ada: `deadlock pulihkan`, `deadlock cegah`, `deadlock deteksi`, `graf`.)
Yang terlihat:
 - Langkah 1 (`deadlock`): dl-1 kunci A lalu B, dl-2 kunci B lalu A. Kedua thread berstatus WAITING, tampil
   Resource Allocation Graph dan "Siklus wait-for: T1 -> T2 -> T1", plus 4 kondisi deadlock.
 - Langkah 2 (jawab y): korban dihentikan, thread lain selesai, "deadlock hilang".
 - Langkah 3 (jawab y): kedua thread mengunci A dulu baru B, keduanya TERMINATED normal.
Alur: tiap thread memegang 1 mutex lalu minta mutex satunya (hold & wait). `mutex_lock` mengubah status jadi
WAITING dan mencatat `wait_on`. Deteksi: ikuti rantai "menunggu mutex milik siapa"; kembali ke awal = siklus.
Recovery: hentikan satu thread dalam siklus (hak root) sehingga kuncinya dilepas.
Pencegahan: semua thread mengambil mutex berurutan (A=1 sebelum B=2) sehingga circular wait mustahil.
Catatan: recovery butuh hak root. Jika kamu sedang `guest`, ketik `masuk root` (password 6s) dulu.

## 6. Alokator Memori Dinamis
Perintah: `uji_memori` (otomatis) atau manual: `memori`, `alokasi 1000`, `memori`, `bebaskan 0`, `memori`,
`alokasi 99999999`
Yang terlihat: angka terpakai/bebas berubah; setelah semua dibebaskan blok bebas menyatu kembali (coalescing);
saat memori habis `kmalloc` mengembalikan NULL dan muncul pesan Out of Memory, kernel TIDAK crash.
Alur: heap 512 KB berisi daftar blok (header + data). Alokasi = first-fit + pecah blok; pembebasan = tandai bebas
lalu gabungkan dengan tetangga bebas.

## 7. Virtual File System (RAMDisk) - CRUD
Perintah: `uji_berkas` (otomatis) atau manual:
    buat a.txt
    tulis a.txt halo
    baca a.txt
    tambah a.txt  dunia
    baca a.txt
    hapus a.txt
    daftar
Yang terlihat: Create, Read, Update (tulis/tambah), Delete berhasil; membaca berkas terhapus -> "Berkas tidak
ditemukan".
Alur: tabel 16 berkas; isi tiap berkas disimpan di heap (kmalloc), diganti/ditambah dengan menyalin ke buffer baru.

## 8. Proteksi & Hak Akses
Perintah: `uji_akses` (otomatis), lalu manual:
    tulis rahasia.txt isi-rahasia
    izin rahasia.txt 60        # pemilik rw, lainnya tidak ada
    masuk guest
    baca rahasia.txt           # DITOLAK
    tulis guest.txt hai        # DIIZINKAN
    hentikan 1                 # DITOLAK (butuh root)
    masuk root                 # password: 6s
Yang terlihat: akses yang diizinkan berhasil, yang tidak diizinkan ditolak ("Akses ditolak"); `uji_akses`
menampilkan LULUS di semua baris.
Alur: setiap operasi berkas memeriksa uid peminta vs pemilik dan bit hak (6=rw, 4=r, 0=tidak ada); root melewati
pemeriksaan. Perintah sistem tertentu (hentikan, ulang, matikan, deadlock pulihkan) hanya untuk root.

---------------------------------------------------------------------
Catatan: nama perintah lama (bahasa Inggris, mis. `help`, `sched`, `ps`) masih diterima sebagai alias.
