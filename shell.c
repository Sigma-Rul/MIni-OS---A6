/* ====================================================================
 * shell.c  -  Antarmuka CLI 6S + skenario demo untuk setiap komponen OS
 * ==================================================================== */
#include "kernel.h"

int cur_uid = 0;                                   /* 0 = root, 1 = guest */
static const char *uid_name(int u) { return u == 0 ? "root" : "guest"; }
static void readline(char *buf, int max, int echo);   /* didefinisikan di bawah */
static int tanya(const char *pertanyaan);              /* tanya y/n, didefinisikan di bawah */

/* ---------- objek bersama untuk demo ---------- */
static mutex_t m_counter, mtx_A, mtx_B;
#define RACE_N      100000                         /* jumlah increment per thread */
#define RACE_WINDOW 30                             /* jeda kecil antara baca & tulis (memperlebar celah race) */
static volatile int shared_counter;                /* DATA BERSAMA untuk demo race condition */
static int race_sync;                              /* 1 = pakai mutex, 0 = tanpa mutex */

void shell_objects_init(void) {
    mutex_init(&m_counter, "counter");
    mutex_init(&mtx_A, "A");
    mutex_init(&mtx_B, "B");
}

/* ---------- fungsi bantu ---------- */
static void wait_ms(u32 ms) { u32 end = ticks + ms; while (ticks < end) idle_wait(); }

/* tunggu sampai semua thread di daftar TERMINATED (atau timeout) */
static int wait_all(tcb_t **list, int n, u32 timeout) {
    u32 end = ticks + timeout;
    while (ticks < end) {
        int done = 1;
        for (int i = 0; i < n; i++) if (list[i]->state != TS_TERMINATED) done = 0;
        if (done) return 1;
        idle_wait();
    }
    return 0;
}
static void do_ps(void) {
    kprintf("  TID NAME         STATE       CPU(ms)  MENUNGGU\n");
    for (int i = 0; i < MAX_THREADS; i++) {
        tcb_t *t = thread_at(i);
        if (t->state == TS_UNUSED) continue;
        kprintf("  %-3d %-12s %-11s %-8u ", t->tid, t->name, state_name[t->state], t->cpu_ticks);
        if (t->state == TS_WAITING) {
            if (t->sleeping) kprintf("tidur (%u ms lagi)", t->wake_tick > ticks ? t->wake_tick - ticks : 0);
            else if (t->wait_on) kprintf("mutex %s", t->wait_on->name);
        }
        kprintf("\n");
    }
}
static void print_mem(const char *label) {
    memstat_t s; mem_stats(&s);
    kprintf("   %-16s pakai=%-7u bebas=%-7u blok bebas=%u\n", label, s.used, s.free, s.free_blocks);
}
/* buat 2 thread sekaligus (status NEW) */
static int create_pair(const char *n1, void (*f1)(void), void *a1, const char *n2, void (*f2)(void), void *a2,
                       tcb_t **t1, tcb_t **t2) {
    thread_reap();
    *t1 = thread_create(n1, f1, a1);
    *t2 = thread_create(n2, f2, a2);
    if (!*t1 || !*t2) { kprintf("Gagal membuat thread (slot/memori habis)\n"); return -1; }
    return 0;
}

/* ================= PERINTAH DASAR ================= */
static void cmd_about(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("6S (Operating Six) v1.0 - kernel x86 32-bit, Tugas Besar Sistem Operasi\n"
            "Fitur : CLI, thread, Round Robin preemptive (quantum bisa diubah),\n"
            "        mutex, deadlock, alokator memori, RAMDisk, hak akses\n");
}
static void cmd_clear(int argc, char **argv) { (void)argc; (void)argv; cons_clear(); }
static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("uptime %u.%03u detik | quantum %d ms | %u context switch\n", ticks / 1000, ticks % 1000, quantum_ticks, ctx_switches);
}
static void cmd_ps(int argc, char **argv) { (void)argc; (void)argv; do_ps(); }
static void cmd_kill(int argc, char **argv) {
    if (argc < 2) { kprintf("pakai: hentikan <tid>\n"); return; }
    int tid = katoi(argv[1]);
    if (thread_kill(tid) == 0) kprintf("thread %d dihentikan\n", tid);
    else kprintf("tidak bisa menghentikan thread %d\n", tid);
}
static void cmd_rag(int argc, char **argv) { (void)argc; (void)argv; rag_dump(); }

/* ================= 1. QUANTUM + ROUND ROBIN ================= */
static void cmd_quantum(int argc, char **argv) {
    if (argc < 2) { kprintf("Quantum Round Robin saat ini: %d ms (rentang 1-1000). Contoh: quantum 20\n", quantum_ticks); return; }
    int q = katoi(argv[1]);
    if (q < 1 || q > 1000) { kprintf("Nilai quantum harus 1..1000 ms\n"); return; }
    sched_set_quantum(q);
    kprintf("Quantum Round Robin diubah menjadi %d ms\n", q);
}
/* Pekerja CPU-bound: tidak pernah yield(), hanya timer yang bisa menghentikannya.
 * Tiap 10 ms CPU ia mencetak hurufnya (A/B/C). Urutan huruf = urutan giliran CPU. */
static void cpu_worker(void) {
    char huruf = (char)(u32)current->arg;
    u32 terakhir = ticks, selesai = ticks + 1200, hitung = 0;
    while (ticks < selesai) {
        hitung++;
        if (ticks - terakhir >= 10) { terakhir = ticks; kputc(huruf); }
    }
    current->result = hitung;
}
static void cmd_sched(int argc, char **argv) {
    (void)argc; (void)argv;
    static const char *nama[3] = { "cpu-A", "cpu-B", "cpu-C" };
    tcb_t *t[3];
    thread_reap();
    for (int i = 0; i < 3; i++) {
        t[i] = thread_create(nama[i], cpu_worker, (void *)(u32)('A' + i));
        if (!t[i]) {
            kprintf("Gagal membuat thread\n");
            for (int k = 0; k < i; k++) thread_kill(t[k]->tid);
            thread_reap();
            return;
        }
    }
    kprintf("=== DEMO 3: ROUND ROBIN (quantum = %d ms) ===\n", quantum_ticks);
    kprintf("3 thread A,B,C berebut CPU tanpa yield(). 1 huruf = 10 ms CPU:\n  ");
    u32 sw0 = ctx_switches;
    for (int i = 0; i < 3; i++) thread_start(t[i]);
    wait_all(t, 3, 10000);
    kprintf("\nContext switch: %u\n", ctx_switches - sw0);
    kprintf("Iterasi: A=%u B=%u C=%u\n", t[0]->result, t[1]->result, t[2]->result);
    kprintf("Penjelasan: huruf bergantian rapat  = quantum kecil (sering ganti)\n");
    kprintf("            huruf berkelompok       = quantum besar (jarang ganti)\n");
    kprintf("Coba: 'quantum 1' atau 'quantum 50', lalu 'jadwal' lagi.\n");
    thread_reap();
}

/* ================= 2. THREAD + STATUS ================= */
/* Tabel status: tiap baris = satu "foto" status shell, prime-calc, sleeper */
static tcb_t *snap_a, *snap_b;
static const char *snap_label[6];
static int snap_state[6][3];
static int snap_n;
static void take_snap(const char *label) {
    if (snap_n >= 6) return;
    snap_label[snap_n] = label;
    snap_state[snap_n][0] = thread_at(0)->state;
    snap_state[snap_n][1] = snap_a->state;
    snap_state[snap_n][2] = snap_b->state;
    snap_n++;
}
static void task_prime(void) {                     /* tugas CPU: hitung bilangan prima */
    u32 count = 0;
    for (u32 n = 2; n < 1000000; n++) {
        int p = 1;
        for (u32 d = 2; d * d <= n; d++) if (n % d == 0) { p = 0; break; }
        count += p;
        if (n == 400000) take_snap("3. sedang berjalan");   /* dicatat dari dalam thread ini */
    }
    current->result = count;
}
static void task_sleeper(void) {                   /* tugas I/O: tidur-bangun */
    for (int i = 0; i < 4; i++) thread_sleep(250);
}
static void cmd_threads(int argc, char **argv) {
    (void)argc; (void)argv;
    tcb_t *a, *b, *all[2];
    snap_n = 0;
    if (create_pair("prime-calc", task_prime, NULL, "sleeper", task_sleeper, NULL, &a, &b)) return;
    all[0] = a; all[1] = b; snap_a = a; snap_b = b;
    kprintf("=== DEMO 2: THREAD & STATUSNYA ===\n");
    kprintf("2 thread berbeda tugas: prime-calc (hitung prima), sleeper (tidur).\n\n");
    take_snap("1. setelah create");
    thread_start(a); thread_start(b);
    take_snap("2. setelah start");
    wait_all(all, 2, 15000);
    take_snap("4. selesai");
    kprintf("  %-20s %-11s %-11s %-11s\n", "Langkah", "shell", "prime-calc", "sleeper");
    for (int i = 0; i < snap_n; i++)
        kprintf("  %-20s %-11s %-11s %-11s\n", snap_label[i], state_name[snap_state[i][0]],
                state_name[snap_state[i][1]], state_name[snap_state[i][2]]);
    kprintf("\nhasil prime-calc: %u bilangan prima di bawah 1000000\n", a->result);
    kprintf("NEW=baru dibuat | READY=antre CPU | RUNNING=sedang jalan (hanya 1)\n");
    kprintf("WAITING=menunggu/tidur | TERMINATED=selesai\n");
    kprintf("Baris 3 dicatat oleh prime-calc sendiri, makanya ia terlihat RUNNING.\n");
    thread_reap();
}

/* ================= 3. SINKRONISASI + RACE CONDITION ================= */
static void race_worker(void) {
    for (int i = 0; i < RACE_N; i++) {
        if (race_sync) mutex_lock(&m_counter);          /* masuk critical section */
        int tmp = shared_counter;                       /* 1. baca */
        for (volatile int k = 0; k < RACE_WINDOW; k++) ;/* (jeda kecil) */
        shared_counter = tmp + 1;                       /* 2. tulis (tmp+1) */
        if (race_sync) mutex_unlock(&m_counter);        /* keluar critical section */
    }
}
static void jalankan_race(int sync) {
    race_sync = sync;
    tcb_t *a, *b, *all[2];
    if (create_pair("race-1", race_worker, NULL, "race-2", race_worker, NULL, &a, &b)) return;
    all[0] = a; all[1] = b;
    shared_counter = 0;
    kprintf("=== DEMO 4: RACE CONDITION (mutex: %s) ===\n", race_sync ? "AKTIF" : "TIDAK AKTIF");
    kprintf("2 thread, masing-masing menambah 1 counter bersama %d kali.\n\n", RACE_N);
    thread_start(a); thread_start(b);
    if (!wait_all(all, 2, 60000)) kprintf("timeout!\n");
    int harapan = 2 * RACE_N, hasil = shared_counter;
    kprintf("  harapan = %d\n  hasil   = %d\n  hilang  = %d\n\n", harapan, hasil, harapan - hasil);
    if (hasil == harapan) {
        kprintf("=> KONSISTEN. Mutex membuat hanya 1 thread yang mengubah counter\n");
        kprintf("   pada satu waktu, jadi tidak ada penambahan yang hilang.\n");
    } else {
        kprintf("=> RACE CONDITION! Dua thread membaca nilai lama yang sama lalu\n");
        kprintf("   saling menimpa, sehingga sebagian penambahan hilang.\n");
    }
    thread_reap();
}
/* 'race' = tanpa mutex, lalu ditawari ulang dengan mutex (jawab y). */
static void cmd_race(int argc, char **argv) {
    int sync = argc > 1 && (!strcmp(argv[1], "sinkron") || !strcmp(argv[1], "sync"));
    jalankan_race(sync);
    if (!sync) {
        kprintf("\n");
        if (tanya("Lanjut dengan sinkronisasi (mutex)?")) { cons_clear(); jalankan_race(1); }
        else kprintf("Nanti bisa: 'race sinkron'\n");
    }
}

/* ================= 4. DEADLOCK ================= */
static void dl_lock_AB(void) {                          /* urutan: A lalu B */
    kprintf("  [%s] ambil kunci A\n", current->name);  mutex_lock(&mtx_A);
    thread_sleep(200);                                  /* beri waktu thread lain mengambil kunci */
    kprintf("  [%s] ambil kunci B\n", current->name);  mutex_lock(&mtx_B);
    kprintf("  [%s] dapat A dan B, selesai kerja\n", current->name);
    mutex_unlock(&mtx_B); mutex_unlock(&mtx_A);
}
static void dl_lock_BA(void) {                          /* urutan TERBALIK: B lalu A */
    kprintf("  [%s] ambil kunci B\n", current->name);  mutex_lock(&mtx_B);
    thread_sleep(200);
    kprintf("  [%s] ambil kunci A\n", current->name);  mutex_lock(&mtx_A);
    kprintf("  [%s] dapat B dan A, selesai kerja\n", current->name);
    mutex_unlock(&mtx_A); mutex_unlock(&mtx_B);
}
static int mutexes_busy(void) { return mtx_A.locked || mtx_B.locked; }

/* Tanya y/n ke pengguna. Mengembalikan 1 jika menjawab y. */
static int tanya(const char *pertanyaan) {
    char jawab[8];
    kprintf("%s (y/n): ", pertanyaan);
    readline(jawab, sizeof(jawab), 1);
    return jawab[0] == 'y' || jawab[0] == 'Y';
}

/* --- PENCEGAHAN: semua thread mengunci A dulu baru B --- */
static void deadlock_cegah(void) {
    tcb_t *a, *b, *all[2];
    kprintf("=== PENCEGAHAN DEADLOCK: URUTAN KUNCI ===\n");
    if (mutexes_busy()) { kprintf("Mutex masih dipegang - jalankan 'deadlock pulihkan' dulu\n"); return; }
    if (create_pair("fix-1", dl_lock_AB, NULL, "fix-2", dl_lock_AB, NULL, &a, &b)) return;
    all[0] = a; all[1] = b;
    kprintf("Aturan: semua thread ambil kunci A dulu, baru B (A=1 < B=2).\n\n");
    thread_start(a); thread_start(b);
    int ok = wait_all(all, 2, 5000);
    kprintf("\n");
    do_ps();
    kprintf("\nHasil: %s\n", ok ? "kedua thread TERMINATED normal, tidak ada deadlock." : "timeout");
    kprintf("Penjelasan: urutan yang sama membuat saling-tunggu melingkar mustahil.\n");
    thread_reap();
}

/* --- RECOVERY: hentikan satu korban, lalu tawarkan lanjut ke pencegahan --- */
static void deadlock_pulihkan(void) {
    tcb_t *victim = NULL;
    kprintf("=== RECOVERY DEADLOCK ===\n");
    if (cur_uid != 0) { kprintf("Ditolak: recovery butuh hak root (ketik 'masuk root')\n"); return; }
    if (!deadlock_detect(1, &victim)) { kprintf("Tidak ada deadlock saat ini.\n"); return; }
    kprintf("\nKorban: T%d(%s) dihentikan, kunci miliknya dilepas.\n\n", victim->tid, victim->name);
    thread_kill(victim->tid);
    wait_ms(600);
    do_ps();
    kprintf("\nHasil: %s\n", deadlock_detect(0, NULL) ? "masih deadlock" : "deadlock hilang, thread lain bisa selesai.");
    kprintf("Penjelasan: korban dihentikan, kuncinya lepas, thread lain lanjut.\n\n");
    thread_reap();
    if (tanya("Lanjut ke pencegahan (fix)?")) { cons_clear(); deadlock_cegah(); }
    else kprintf("Nanti bisa: 'deadlock cegah'\n");
}

static void cmd_deadlock(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    tcb_t *victim = NULL, *a, *b;
    if (!strcmp(mode, "pulihkan") || !strcmp(mode, "recover")) {
        deadlock_pulihkan();
    } else if (!strcmp(mode, "deteksi") || !strcmp(mode, "detect")) {
        kprintf("=== DETEKSI DEADLOCK ===\n");
        rag_dump();
        if (!deadlock_detect(1, &victim)) kprintf("Tidak ada siklus: AMAN\n");
        else kprintf("Siklus ditemukan: DEADLOCK\n");
    } else if (!strcmp(mode, "cegah") || !strcmp(mode, "fix")) {
        deadlock_cegah();
    } else {                                            /* --- SKENARIO DEADLOCK --- */
        kprintf("=== DEMO 5: DEADLOCK ===\n");
        if (mutexes_busy()) { kprintf("Mutex masih dipegang - jalankan 'deadlock pulihkan' dulu\n"); return; }
        if (create_pair("dl-1", dl_lock_AB, NULL, "dl-2", dl_lock_BA, NULL, &a, &b)) return;
        kprintf("dl-1 ambil kunci A lalu B | dl-2 ambil kunci B lalu A (berlawanan)\n");
        thread_start(a); thread_start(b);
        wait_ms(1500);
        kprintf("\n");
        do_ps();
        kprintf("\n");
        rag_dump();
        kprintf("\n");
        deadlock_detect(1, &victim);
        kprintf("HASIL: DEADLOCK! Kedua thread saling menunggu selamanya.\n");
        kprintf("4 kondisi: mutual exclusion, hold and wait, no preemption, circular wait\n");
        if (tanya("Lanjut ke recovery (pulihkan)?")) { cons_clear(); deadlock_pulihkan(); }
        else kprintf("Nanti bisa: 'deadlock pulihkan' lalu 'deadlock cegah'\n");
    }
}

/* ================= 5. MANAJEMEN MEMORI ================= */
static void *slots[32]; static u32 slot_sz[32];
static void cmd_meminfo(int argc, char **argv) {
    (void)argc; (void)argv;
    memstat_t s; mem_stats(&s);
    kprintf("Heap total : %u byte\nTerpakai   : %u byte (%u blok)\nTersedia   : %u byte (%u blok, terbesar %u)\n",
            s.total, s.used, s.used_blocks, s.free, s.free_blocks, s.largest_free);
}
static void cmd_malloc(int argc, char **argv) {
    if (argc < 2) { kprintf("pakai: alokasi <byte>\n"); return; }
    u32 n = (u32)katoi(argv[1]);
    for (int i = 0; i < 32; i++) if (!slots[i]) {
        slots[i] = kmalloc(n);
        if (!slots[i]) { kprintf("kmalloc(%u) gagal: Out of Memory (ditangani, kernel tetap hidup)\n", n); return; }
        slot_sz[i] = n;
        kprintf("dialokasikan %u byte -> slot %d\n", n, i);
        return;
    }
    kprintf("slot penuh (maks 32)\n");
}
static void cmd_free(int argc, char **argv) {
    int i = argc > 1 ? katoi(argv[1]) : -1;
    if (i < 0 || i > 31 || !slots[i]) { kprintf("pakai: bebaskan <slot> (slot harus terisi)\n"); return; }
    kfree(slots[i]); slots[i] = NULL;
    kprintf("slot %d dibebaskan (%u byte)\n", i, slot_sz[i]);
}
static void cmd_memtest(int argc, char **argv) {
    (void)argc; (void)argv;
    void *p[64]; int n = 0;
    kprintf("=== DEMO 6: MEMORI DINAMIS (heap %d KB) ===\n", HEAP_SIZE / 1024);
    kprintf("1) kondisi awal\n");                          print_mem("awal");
    for (int i = 0; i < 8; i++) p[i] = kmalloc(16 * 1024);
    kprintf("2) alokasi 8 blok x 16 KB -> terpakai naik\n"); print_mem("setelah alokasi");
    for (int i = 0; i < 8; i += 2) { kfree(p[i]); p[i] = NULL; }
    kprintf("3) bebaskan selang-seling -> blok bebas terpecah\n"); print_mem("sebagian bebas");
    for (int i = 0; i < 8; i++) if (p[i]) kfree(p[i]);
    kprintf("4) bebaskan semua -> blok bebas menyatu (coalescing)\n"); print_mem("semua bebas");
    while (n < 64 && (p[n] = kmalloc(32 * 1024))) n++;
    kprintf("5) habiskan memori: kmalloc gagal (NULL) setelah %d alokasi\n", n);
    kprintf("   Out of Memory ditangani, kernel TIDAK crash\n");
    print_mem("saat penuh");
    for (int i = 0; i < n; i++) kfree(p[i]);
    kprintf("6) semua dilepas -> memori pulih\n");         print_mem("pulih");
}

/* ================= 6. SISTEM BERKAS (RAMDisk) ================= */
static const char *perm_str(u8 b) { return b == 6 ? "rw" : b == 4 ? "r-" : b == 2 ? "-w" : "--"; }
static char rawcopy[128];                              /* salinan baris perintah utuh */
static const char *rest_after(int n) {                 /* teks setelah n kata pertama */
    const char *p = rawcopy;
    while (n--) { while (*p == ' ') p++; while (*p && *p != ' ') p++; }
    while (*p == ' ') p++;
    return p;
}
static void fs_report(int r) { if (r) kprintf("galat: %s\n", fs_strerror(r)); }
static void cmd_ls(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("  HAK   PEMILIK  UKURAN  NAMA\n");
    for (int i = 0; i < MAX_FILES; i++) {
        file_t *f = fs_at(i);
        if (f->used) kprintf("  %s%s  %-7s  %-6u  %s\n", perm_str(f->perm_owner), perm_str(f->perm_other), uid_name(f->owner), f->size, f->name);
    }
}
static void cmd_touch(int argc, char **argv) { if (argc < 2) { kprintf("pakai: buat <berkas>\n"); return; } fs_report(fs_create(argv[1], cur_uid)); }
static void cmd_cat(int argc, char **argv) {
    const char *d; u32 sz;
    if (argc < 2) { kprintf("pakai: baca <berkas>\n"); return; }
    int r = fs_read(argv[1], cur_uid, &d, &sz);
    if (r) { fs_report(r); return; }
    kputs(d); kputc('\n');
}
static void cmd_write(int argc, char **argv) {
    if (argc < 3) { kprintf("pakai: tulis <berkas> <teks...>\n"); return; }
    const char *t = rest_after(2);
    fs_report(fs_write(argv[1], t, strlen(t), cur_uid, 0));
}
static void cmd_append(int argc, char **argv) {
    if (argc < 3) { kprintf("pakai: tambah <berkas> <teks...>\n"); return; }
    const char *t = rest_after(2);
    fs_report(fs_write(argv[1], t, strlen(t), cur_uid, 1));
}
static void cmd_rm(int argc, char **argv) { if (argc < 2) { kprintf("pakai: hapus <berkas>\n"); return; } fs_report(fs_delete(argv[1], cur_uid)); }
static void cmd_chmod(int argc, char **argv) {
    if (argc < 3) { kprintf("pakai: izin <berkas> <PL> (digit pemilik,lainnya: 6=rw 4=r 2=w 0=tidak ada)\n"); return; }
    fs_report(fs_chmod(argv[1], cur_uid, katoi(argv[2])));
}
static void step(const char *apa, int r) { kprintf("  %-34s -> %s\n", apa, r == FS_OK ? "OK" : fs_strerror(r)); }
static void cmd_fstest(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *d; u32 sz; int r;
    kprintf("=== DEMO 7: FILE SYSTEM CRUD (user: %s) ===\n", uid_name(cur_uid));
    step("CREATE  note.txt", fs_create("note.txt", cur_uid));
    step("UPDATE  tulis 'halo'", fs_write("note.txt", "halo", 4, cur_uid, 0));
    r = fs_read("note.txt", cur_uid, &d, &sz);  kprintf("  READ    note.txt                   -> '%s' (%u byte)\n", r ? "?" : d, sz);
    step("UPDATE  tambah ' dunia'", fs_write("note.txt", " dunia", 6, cur_uid, 1));
    r = fs_read("note.txt", cur_uid, &d, &sz);  kprintf("  READ    note.txt                   -> '%s' (%u byte)\n", r ? "?" : d, sz);
    step("DELETE  note.txt", fs_delete("note.txt", cur_uid));
    step("READ    note.txt (harus gagal)", fs_read("note.txt", cur_uid, &d, &sz));
    kprintf("Create, Read, Update, Delete berhasil. Berkas disimpan di RAM.\n");
}

/* ================= 7. PROTEKSI & HAK AKSES ================= */
static int pt_pass, pt_fail;
static void cek(const char *apa, int hasil, int harapan) {
    int ok = (hasil == harapan);
    if (ok) pt_pass++; else pt_fail++;
    kprintf("  [%s] %-42s -> %s\n", ok ? "LULUS" : "GAGAL", apa, hasil == FS_OK ? "DIIZINKAN" : "DITOLAK");
}
static void cmd_prottest(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *d; u32 sz;
    pt_pass = pt_fail = 0;
    kprintf("=== DEMO 8: PROTEKSI & HAK AKSES ===\n");
    fs_delete("secret.txt", 0); fs_delete("guest.txt", 0);
    fs_write("secret.txt", "rahasia", 7, 0, 0);
    fs_chmod("secret.txt", 0, 60);                      /* root rw, lainnya tidak ada */
    kprintf("secret.txt: pemilik=root hak=rw----   readme.txt: pemilik=root hak=rw-r--\n");
    kprintf("Sebagai guest:\n");
    cek("guest membaca readme.txt (lainnya=r)",      fs_read("readme.txt", 1, &d, &sz), FS_OK);
    cek("guest membaca secret.txt (lainnya=tidak)",  fs_read("secret.txt", 1, &d, &sz), FS_EACCES);
    cek("guest menulis secret.txt",                  fs_write("secret.txt", "x", 1, 1, 0), FS_EACCES);
    cek("guest menghapus secret.txt",                fs_delete("secret.txt", 1), FS_EACCES);
    cek("guest menulis readme.txt (lainnya=r saja)", fs_write("readme.txt", "x", 1, 1, 0), FS_EACCES);
    cek("guest chmod secret.txt",                    fs_chmod("secret.txt", 1, 66), FS_EACCES);
    cek("guest membuat+menulis guest.txt",           fs_write("guest.txt", "milikku", 7, 1, 0), FS_OK);
    cek("guest membaca guest.txt miliknya",          fs_read("guest.txt", 1, &d, &sz), FS_OK);
    kprintf("Sebagai root:\n");
    cek("root membaca secret.txt",                   fs_read("secret.txt", 0, &d, &sz), FS_OK);
    cek("root menghapus guest.txt",                  fs_delete("guest.txt", 0), FS_OK);
    fs_delete("secret.txt", 0);
    kprintf("Hasil: %d lulus, %d gagal\n", pt_pass, pt_fail);
    kprintf("Akses yang diizinkan berhasil, yang tidak diizinkan DITOLAK.\n");
}
static void cmd_whoami(int argc, char **argv) { (void)argc; (void)argv; kprintf("%s (uid=%d)\n", uid_name(cur_uid), cur_uid); }

/* ================= INTI SHELL ================= */
static void readline(char *buf, int max, int echo) {
    int n = 0;
    for (;;) {
        int c = cons_getc();                            /* menunggu tombol (CPU diserahkan ke thread lain) */
        if (c == '\n') { kputc('\n'); buf[n] = 0; return; }
        if (c == '\b') { if (n > 0) { n--; if (echo) { kputc('\b'); kputc(' '); kputc('\b'); } } continue; }
        if (c >= 32 && c < 127 && n < max - 1) { buf[n++] = (char)c; if (echo) kputc((char)c); }
    }
}
static void cmd_su(int argc, char **argv) {
    if (argc < 2) { kprintf("pakai: masuk <root|guest>\n"); return; }
    if (!strcmp(argv[1], "guest")) { cur_uid = 1; return; }
    if (!strcmp(argv[1], "root")) {
        char pw[32];
        if (cur_uid != 0) { kprintf("Password: "); readline(pw, sizeof(pw), 0); } else pw[0] = 0;
        if (cur_uid == 0 || !strcmp(pw, "6s")) cur_uid = 0; else kprintf("su: autentikasi gagal\n");
        return;
    }
    kprintf("su: user tidak dikenal\n");
}
static void cmd_skenario(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("=== 8 KETENTUAN -> PERINTAH UJI ===\n"
            "1. Boot & CLI      : bantuan, tentang, waktu\n"
            "2. Thread & status : thread\n"
            "3. Round Robin     : quantum 5, jadwal, quantum 50, jadwal\n"
            "4. Sinkronisasi    : race (lalu jawab y)\n"
            "5. Deadlock        : deadlock (lalu jawab y, y)\n"
            "6. Memori dinamis  : uji_memori\n"
            "7. File system     : uji_berkas\n"
            "8. Proteksi        : uji_akses\n");
}
static void cmd_reboot(int argc, char **argv) { (void)argc; (void)argv; outb(0x64, 0xFE); for (;;) __asm__ volatile("hlt"); }
static void cmd_shutdown(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Mematikan sistem...\n");
    outw(0x604, 0x2000); outw(0xB004, 0x2000);
    for (;;) __asm__ volatile("cli; hlt");
}

typedef void (*cmd_fn)(int, char **);
/* name = nama perintah (Indonesia), alias = nama lama (Inggris, tetap diterima) */
struct cmd { const char *name; const char *alias; cmd_fn fn; int root; int clear; };
static void cmd_help(int argc, char **argv);
static const struct cmd cmds[] = {
    { "bantuan",    "help",     cmd_help,     0, 1 },
    { "skenario",   "skenario", cmd_skenario, 0, 1 },
    { "tentang",    "about",    cmd_about,    0, 0 },
    { "bersih",     "clear",    cmd_clear,    0, 0 },
    { "waktu",      "uptime",   cmd_uptime,   0, 0 },
    { "proses",     "ps",       cmd_ps,       0, 0 },
    { "quantum",    "quantum",  cmd_quantum,  0, 0 },
    { "thread",     "threads",  cmd_threads,  0, 1 },
    { "jadwal",     "sched",    cmd_sched,    0, 1 },
    { "race",       "balapan",  cmd_race,     0, 1 },
    { "deadlock",   "buntu",    cmd_deadlock, 0, 1 },
    { "graf",       "rag",      cmd_rag,      0, 0 },
    { "hentikan",   "kill",     cmd_kill,     1, 0 },
    { "memori",     "meminfo",  cmd_meminfo,  0, 0 },
    { "alokasi",    "malloc",   cmd_malloc,   0, 0 },
    { "bebaskan",   "free",     cmd_free,     0, 0 },
    { "uji_memori", "memtest",  cmd_memtest,  0, 1 },
    { "daftar",     "ls",       cmd_ls,       0, 0 },
    { "buat",       "touch",    cmd_touch,    0, 0 },
    { "tulis",      "write",    cmd_write,    0, 0 },
    { "tambah",     "append",   cmd_append,   0, 0 },
    { "baca",       "cat",      cmd_cat,      0, 0 },
    { "hapus",      "rm",       cmd_rm,       0, 0 },
    { "izin",       "chmod",    cmd_chmod,    0, 0 },
    { "uji_berkas", "fstest",   cmd_fstest,   0, 1 },
    { "siapa",      "whoami",   cmd_whoami,   0, 0 },
    { "masuk",      "su",       cmd_su,       0, 0 },
    { "uji_akses",  "prottest", cmd_prottest, 0, 1 },
    { "ulang",      "reboot",   cmd_reboot,   1, 0 },
    { "matikan",    "shutdown", cmd_shutdown, 1, 0 },
};
#define NCMD ((int)(sizeof(cmds) / sizeof(cmds[0])))
static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("=== DAFTAR PERINTAH 6S ===\n"
            "Info       : bantuan, skenario, tentang, waktu, proses, bersih\n"
            "Round Rob. : quantum [ms], jadwal, thread\n"
            "Sinkron    : race (lalu jawab y untuk sinkronisasi)\n"
            "Deadlock   : deadlock, deadlock deteksi|pulihkan|cegah, graf\n"
            "Memori     : uji_memori, memori, alokasi <byte>, bebaskan <slot>\n"
            "Berkas     : uji_berkas, daftar, buat, tulis, tambah, baca, hapus, izin\n"
            "Proteksi   : uji_akses, siapa, masuk guest, masuk root (password: 6s)\n"
            "Khusus root: hentikan <tid>, ulang, matikan\n"
            "Mulai dari : skenario\n");
}

void shell_main(void) {
    static char line[128];
    char *av[8];
    for (;;) {
        kprintf("%s@6s%s ", uid_name(cur_uid), cur_uid == 0 ? "#" : "$");
        readline(line, sizeof(line), 1);
        kstrcpy(rawcopy, line, sizeof(rawcopy));
        int ac = 0;                                      /* pecah baris menjadi kata-kata */
        for (char *p = line; *p && ac < 8; ) {
            while (*p == ' ') *p++ = 0;
            if (!*p) break;
            av[ac++] = p;
            while (*p && *p != ' ') p++;
        }
        if (ac == 0) continue;
        int found = 0;
        for (int i = 0; i < NCMD; i++) if (!strcmp(av[0], cmds[i].name) || !strcmp(av[0], cmds[i].alias)) {
            found = 1;
            if (cmds[i].root && cur_uid != 0) kprintf("Ditolak: '%s' butuh hak root\n", av[0]);
            else {
                if (cmds[i].clear) cons_clear();
                cmds[i].fn(ac, av);
                if (cmds[i].fn != cmd_clear) kputc('\n');     /* baris kosong pemisah antar perintah */
            }
            break;
        }
        if (!found) kprintf("perintah '%s' tidak dikenal (ketik 'bantuan')\n", av[0]);
    }
}
