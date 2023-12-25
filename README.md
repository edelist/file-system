This is a simple file system implementation in C that uses the FAT 
allocation scheme on top of a virtual disk. There are 13 functions: make_fs, mount_fs, unount_fs,
fs_open, fs_close, fs_create, fs_delete, fs_read, fs_write, fs_get_filesize, fs_listfiles,
fs_lseek, fs_truncate. It is intended to store 64 files, has 8192 blocks available on disk,
and only 4096 are reserved as data blocks. The maximum file size is 16 MB. 