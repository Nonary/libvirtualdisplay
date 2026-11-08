#!/usr/bin/env python3
"""Execute the production FD transaction helpers with a deterministic FD table."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'linux/vibeshine-drm'
SOURCE = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
struct file { int refs; };
static int states[32], deny, installed;
static int get_unused_fd_flags(int flags) {
    assert(flags == 1);
    if (deny) return -EMFILE;
    for (int i=0;i<32;i++) if (!states[i]) { states[i]=1; return i; }
    return -EMFILE;
}
static struct file *get_file(struct file *file) { file->refs++; return file; }
static void fput(struct file *file) { assert(file->refs > 1); file->refs--; }
static void put_unused_fd(int fd) { assert(states[fd] == 1); states[fd]=0; }
static void fd_install(int fd, struct file *file) {
    assert(states[fd] == 1 && file->refs > 1); states[fd]=2; installed++;
}
#include "vkms_frame_fds.h"
int main(void) {
    struct file file = {1};
    struct vkms_frame_fds fds = {0};
    for (int i=0;i<VKMS_FRAME_MAX_FDS;i++)
        assert(vkms_frame_reserve_fd(&fds, &file) == i);
    assert(file.refs == 9 && installed == 0);
    assert(vkms_frame_reserve_fd(&fds, &file) == -EOVERFLOW);
    /* Failed copyout must discard every reserved FD and reference. */
    vkms_frame_finish_fds(&fds, false);
    assert(file.refs == 1 && fds.count == 0 && installed == 0);
    for (int i=0;i<32;i++) assert(states[i] == 0);
    assert(vkms_frame_reserve_fd(&fds, &file) == 0);
    deny=1;
    assert(vkms_frame_reserve_fd(&fds, &file) == -EMFILE);
    assert(fds.count == 1 && file.refs == 2);
    vkms_frame_finish_fds(&fds, false);
    assert(file.refs == 1 && states[0] == 0);
    deny=0;
    assert(vkms_frame_reserve_fd(&fds, &file) == 0);
    assert(vkms_frame_reserve_fd(&fds, &file) == 1);
    assert(installed == 0);
    /* Only a successful complete copyout publishes descriptors. */
    vkms_frame_finish_fds(&fds, true);
    assert(installed == 2 && fds.count == 0 && file.refs == 3);
    assert(states[0] == 2 && states[1] == 2);
    fput(&file); fput(&file);
    assert(file.refs == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vibeshine-frame-fds-') as name:
    path=Path(name); (path/'linux').mkdir()
    for header in ['fdtable.h','file.h']:
        (path/'linux'/header).write_text('')
    (path/'linux/fcntl.h').write_text('#define O_CLOEXEC 1\n')
    (path/'test.c').write_text(SOURCE)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-I',str(path),'-I',str(DRIVER.resolve()),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Frame FD transaction: rollback, exhaustion, bounds and publication passed')
