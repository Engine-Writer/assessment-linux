#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

#include <termios.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <endian.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>

#define MAX_TTYS    64
#define MAX_SERIALS 4

struct gpt_entry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
} __attribute__((packed));

static uint16_t label_root[36] = { 'p','r','i','m','a','r','y',0 };
static uint16_t label_efi[36]  = { 'E','S','P',0 };

typedef struct {
    const char *path;
    const char *type;
    int mode;
} mount_t;

static int label_cmp(const uint16_t *a, const uint16_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

static int scan_gpt(const char *dev, const uint16_t *want) {
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open gpt dev"); return -1; }

    if (lseek(fd, 512LL * 2, SEEK_SET) < 0) { perror("lseek gpt"); close(fd); return -1; }

    struct gpt_entry e;
    for (int i = 0; i < 128; i++) {
        ssize_t r = read(fd, &e, sizeof(e));
        if (r != (ssize_t)sizeof(e)) break;

        uint16_t nm[36];
        memcpy(nm, e.name, sizeof(nm));

        if (e.first_lba != 0 && label_cmp(want, nm, 36) == 0) {
            close(fd);
            return i + 1;
        }
    }
    close(fd);
    return -1;
}

static int match_sh(const char buf[2]) {
    return tolower((unsigned char)buf[0]) == 's' && tolower((unsigned char)buf[1]) == 'h';
}


static void try_mount_root(void) {
    mount_t early[] = {
        {"/proc", "proc", 0555},
        {"/sys", "sysfs", 0555},
        {"/dev", "devtmpfs", 0755},
    };
    for (size_t i = 0; i < sizeof(early)/sizeof(early[0]); i++) {
        mkdir(early[i].path, early[i].mode);
        mount(early[i].type, early[i].path, early[i].type, 0, NULL);
    }

    int part = scan_gpt("/dev/sda", label_root);
    if (part < 0) return;

    char rootdev[64];
    snprintf(rootdev, sizeof(rootdev), "/dev/sda%d", part);

    mkdir("/mnt", 0755);
    if (mount(rootdev, "/mnt", "ext4", 0, NULL) != 0) return;

    mount_t into_new[] = {
        {"/mnt/proc", "proc", 0555},
        {"/mnt/sys", "sysfs", 0555},
        {"/mnt/dev", "devtmpfs", 0755},
    };
    for (size_t i = 0; i < sizeof(into_new)/sizeof(into_new[0]); i++) {
        mkdir(into_new[i].path, into_new[i].mode);
        mount(into_new[i].type, into_new[i].path, into_new[i].type, 0, NULL);
    }

    mkdir("/mnt/bin", 0755);
    int src = open("/bin/busybox", O_RDONLY | O_CLOEXEC);
    if (src >= 0) {
        int dst = open("/mnt/bin/busybox", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
        if (dst >= 0) {
            char buf[8192];
            for (;;) {
                ssize_t r = read(src, buf, sizeof(buf));
                if (r <= 0) break;
                ssize_t w = write(dst, buf, r);
                if (w != r) break;
            }
            close(dst);
        }
        close(src);
    }

    if (chroot("/mnt") != 0) return;
    if (chdir("/") != 0) return;
    

    if (access("/bin/busybox", X_OK) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/busybox", "/bin/busybox", "--install", "-s", "/bin", NULL);
            _exit(1);
        } else if (pid > 0) waitpid(pid, NULL, 0);
    }

    setenv("PATH", "/bin:/usr/bin", 1);

    int efi_part = scan_gpt("/dev/sda", label_efi);
    if (efi_part > 0) {
        char efidev[64];
        snprintf(efidev, sizeof(efidev), "/dev/sda%d", efi_part);
        mkdir("/boot", 0755);
        mkdir("/boot/efi", 0755);
        mount(efidev, "/boot/efi", "vfat", 0, NULL);
    }

    if (access("/boot/initramfs.cpio.gz", R_OK) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/busybox", "sh", "-c",
                  "gzip -dc /boot/initramfs.cpio.gz | cpio -id", (char*)NULL);
            _exit(127);
        } else if (pid > 0) waitpid(pid, NULL, 0);
    }
}


int lsdir(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        perror("opendir");
        return 1;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        printf("%s\n", entry->d_name);
    }

    closedir(d);
    return 0;
}

int main() {
    // create devices
    mknod("/dev/tty0",      S_IFCHR | 0620, makedev(4,0));
    mknod("/dev/ttyS0",     S_IFCHR | 0620, makedev(4,64));
    mknod("/dev/null",      S_IFCHR | 0666, makedev(1,3));
    mknod("/dev/zero",      S_IFCHR | 0666, makedev(1,5));
    mknod("/dev/random",    S_IFCHR | 0666, makedev(1,8));
    mknod("/dev/urandom",   S_IFCHR | 0666, makedev(1,9));

    try_mount_root();
    
    // install BusyBox symlinks
    pid_t pid = fork();
    if(pid == 0) {
        execl("/bin/busybox", "/bin/busybox", "--install", "-s", "/bin", NULL);
        _exit(1); // if exec fails
    } else if(pid > 0) {
        wait(NULL);
    }

    int tty0_fd = open("/dev/tty0", O_RDWR);
    int ttyS0_fd = open("/dev/ttyS0", O_RDWR);

    if (tty0_fd < 0 || ttyS0_fd < 0) {
        perror("Failed to open consoles");
        return 1;
    }

    // startup messages
    const char *msgs[] = {
        "hello world\n",
        "[press Ctrl+A then X to exit if QEMU_GRAPHICAL is false (The default for QEMU_GRAPHICAL in the build.sh script)]\n",
        "[type 'sh' to access busybox ash shell]\n"
    };
    int msgs_to_display = access("/bin/busybox", X_OK) == 0 ? 3:2;
    for (int i=0; i<msgs_to_display; i++) {
        write(tty0_fd, msgs[i], strlen(msgs[i]));
        write(ttyS0_fd, msgs[i], strlen(msgs[i]));
    }

    while (access("/bin/busybox", X_OK) != 0) {sleep(10);} // Not much else to do if we don't have busybox

    char input0[2]={0}, input1[2]={0};
    char c;
    setenv("PATH", "/bin:/usr/bin", 1);

    /* For anyone unaware why or how these flags came to be, it was due to a race condition which occurs
    because the linux kernel passes execution between me and busybox. When we gain execution we can "steal"
    keypresses from busybox. These booleans prevent this from occuring */
    bool ttyS0_active=false, tty0_active=false;

    while(1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(tty0_fd, &rfds);
        FD_SET(ttyS0_fd, &rfds);
        int maxfd = (tty0_fd > ttyS0_fd) ? tty0_fd:ttyS0_fd;

        if (select(maxfd+1, &rfds, NULL, NULL, NULL) > 0 && access("/bin/busybox", X_OK) == 0) {
            if (FD_ISSET(tty0_fd, &rfds) && !tty0_active) {
                if (read(tty0_fd, &c, 1) > 0) {
                    input0[0] = input0[1];
                    input0[1] = c;
                    if (match_sh(input0)) {
                        tty0_active = true;
                        write(tty0_fd, "Launching shell on tty0...\n", 28);
                        if (fork() == 0) {
                            dup2(tty0_fd, 0);
                            dup2(tty0_fd, 1);
                            dup2(tty0_fd, 2);
                            execl("/bin/busybox", "sh", NULL);
                            _exit(1);
                        }
                        input0[0] = input0[1] = 0;
                    }
                }
            }
            if (FD_ISSET(ttyS0_fd, &rfds) && !ttyS0_active) {
                if (read(ttyS0_fd, &c, 1) > 0) {
                    input1[0] = input1[1];
                    input1[1] = c;
                    if (match_sh(input1)) {
                        ttyS0_active = true;
                        write(ttyS0_fd, "Launching shell on ttyS0...\n", 28);
                        if (fork()==0) {
                            setsid();
                            ioctl(ttyS0_fd, TIOCSCTTY, 1);
                            dup2(ttyS0_fd, 0);
                            dup2(ttyS0_fd, 1);
                            dup2(ttyS0_fd, 2);
                            execl("/bin/busybox", "sh", NULL);
                            perror("execl failed: ");  // Should not reach here
                            _exit(1);
                        }
                        input1[0] = input1[1] = 0;
                    }
                }
            }
        }
    }

    return 0;
}
