#include <stdlib.h>
#include <stdio.h>
#include "disk.h"
#include <sys/types.h>
#define MAX_F_NAME 15
#define MAX_FILDES 32
#define MAX_FILES 64
#define FAT_SIZE 8192
#define FREE -1

// Define min function
static inline int min(int a, int b) {
        return (a < b) ? a : b;
}

// Define max function
static inline int max(int a, int b) {
        return (a > b) ? a : b;
}

// superblock
struct super_block {
        int fat_idx; // first block of FAT
        int fat_len; // length of FAT in blocks
        int dir_idx; // first block of directory
        int dir_len; // length of directory in blocks
        int data_idx; // first block of file-data
};

// directory entry
struct dir_entry {
        int used; // is this file-"slot" in use
        char name [MAX_F_NAME + 1]; // DOH!
        int size; // file size
        int head; // first data block of file
        int ref_cnt; // how many open file descriptors are there? ref_cnt > 0 -> cannot delete
};

// file descriptor
struct file_descriptor {
        int used; // fd in use
        //struct dir_entry* file_entry; // pointer to directory entry
        int file; // first block of the file fd refers to
        int offset; // position of fd within f
};

// global variables
struct super_block fs;
struct file_descriptor global_fd_array[MAX_FILDES];
int* FAT;
struct dir_entry* DIR;

int make_fs(char* disk_name) {
        // create, open disk
        if (make_disk(disk_name) == -1 || open_disk(disk_name) == -1) {
                return -1;
        }

        // initialize and write superblock
        fs.fat_idx = 2; // superblock at block 0
        fs.dir_len = 1;
        fs.fat_len = 8; // FAT is 8 blocks
        fs.dir_idx = 1;
        fs.data_idx = 10;

        // init FAT and DIR in memory
        int* FAT = (int*)calloc(FAT_SIZE, sizeof(int));
        if (!FAT) {
                close_disk();
                return -1;
        }

        // set all entries in FAT to FREE
        int i;
        for (i=0; i<FAT_SIZE; i++) {
                FAT[i] = FREE;
        }

        // allocate and init DIR
        struct dir_entry* DIR = (struct dir_entry*)calloc(MAX_FILES, sizeof(struct dir_entry));
        if (!DIR) {
                free(FAT);
                close_disk();
                return -1;
        }

        block_write(0, (char*)&fs);

        for (i=0; i<MAX_FILES; i++) {
                DIR[i].used = 0;
                DIR[i].head = -1;
                DIR[i].size = 0;
                DIR[i].ref_cnt = 0;
        }

        for (i=0; i<fs.fat_len; i++) {
                block_write(fs.fat_idx+i, (char*)&FAT[i * BLOCK_SIZE / sizeof(int)]);
        }
        block_write(fs.dir_idx, (char*)&DIR);


        // free allocated memory
        free(FAT);
        free(DIR);

        // close disk
        if (close_disk() == -1) {
                return -1;
        }

        return 0;
}

int mount_fs(char* disk_name) {

        // open disk
        if (open_disk(disk_name) == -1) {
                perror("ERROR: mount_fs failed to open disk.");
                return -1;
        }

        // load superblock
        if (block_read(0, (char*)&fs) == -1) {
                perror("ERROR: mount_fs failed to read superblock");
                close_disk();
                return -1;
        }

        // load FAT
        FAT = (int*)malloc(FAT_SIZE * sizeof(int));
        if (!FAT) {
                perror("ERROR: mount_fs failed to allocate memory fot FAT.");
                close_disk();
                return -1;
        }

        if (block_read(fs.fat_idx, (char*)FAT) == -1) {
                free(FAT);
                close_disk();
                return -1; // failed to read FAT
        }

        // load directory
        DIR = (struct dir_entry*)malloc(MAX_FILES * sizeof(struct dir_entry));
        if (!DIR) {
                perror("ERROR: mount_fs failed to allocate memory for directory.");
                free(FAT);
                close_disk();
                return -1;
        }

        if (block_read(fs.dir_idx, (char*)DIR) == -1) {
                perror("ERROR: mount_fs failed to read directory.");
                free(FAT);
                free(DIR);
                close_disk();
                return -1;
        }

        return 0;
}

int umount_fs(char* disk_name) {

        // write back FAT
        if (FAT) {
                if (block_write(fs.fat_idx, (char*)FAT) == -1) {
                        free(FAT);
                        free(DIR);
                        close_disk();
                        return -1; // failed to write FAT
                }
                free(FAT);
        }

        // write back DIR
        if (DIR) {
                if (block_write(fs.dir_idx, (char*)DIR) == -1) {
                        free(DIR);
                        close_disk();
                        return -1; // failed to write DIR
                }
                free(DIR);
        }

        // close disk
        if (close_disk() == -1) {
                return -1; // failed to close disk
        }

        return 0;
}

int fs_open(char* name) {
        // check if file exists
        int file_index = -1;
        int i;
        for (i=0; i<MAX_FILES; i++) {
                if (DIR[i].used && strcmp(DIR[i].name, name) == 0) {
                        file_index = i;
                        break;
                }
        }

        // handle file not found or previously deleted files
        if (file_index == -1) {
                perror("ERROR: fs_open file not found.");
                return -1;
        }

        // check for available file descriptor
        int fd_index = -1;
        for (i=0; i<MAX_FILDES; i++) {
                if (!global_fd_array[i].used) {
                        fd_index = i;
                        break;
                }
        }

        // handle case where there are no available file descriptors
        if (fd_index == -1) {
                perror("ERROR: fs_open no available file descriptor.");
                return -1;
        }

        // allocate and initialize file descriptor
        global_fd_array[fd_index].used = 1;
        global_fd_array[fd_index].file = file_index;
        global_fd_array[fd_index].offset = 0;

        // increment ref count for file
        DIR[file_index].ref_cnt++;

        return fd_index;
}

int fs_close(int fildes) {
        // validate file descriptor
        if (fildes<0 || fildes>=MAX_FILDES || !global_fd_array[fildes].used) {
                perror("ERROR: fs_close invalid file descriptor.");
                return -1;
        }

        // retrieve directory entry for file
        int file_index = global_fd_array[fildes].file;
        if (file_index <0 || file_index >= MAX_FILES || !DIR[file_index].used) {
                return -1; // file not found or already deleted
        }

        // close file descriptor
        global_fd_array[fildes].used = 0;
        global_fd_array[fildes].offset = 0;

        // decrease ref count
        DIR[file_index].ref_cnt--;

        return 0;
}

int fs_create(char* name) {
        // check file name length
        if (strlen(name) > MAX_F_NAME) {
                perror("ERROR: fs_create file name too long.");
                return -1;
        }

        // check for existing file
        int i;
        for (i=0; i<MAX_FILES; i++) {
                if (DIR[i].used && strcmp(DIR[i].name, name) == 0) {
                        perror("ERROR: fs_create file already exists.");
                        return -1;
                }
        }

        // find free directory slot
        int free_index = -1;
        for (i=0; i<MAX_FILES; i++) {
                if (!DIR[i].used) {
                        free_index = i;
                        break;
                }
        }

        if (free_index == -1) {
                perror("ERROR: fs_create no free directory slots.");
                return -1;
        }

        // create file
        DIR[free_index].used = 1;
        strncpy(DIR[free_index].name, name, MAX_F_NAME);
        DIR[free_index].name[MAX_F_NAME] = '\0'; // ensure NULL termination
        DIR[free_index].size = 0;
        DIR[free_index].head = FREE;
        DIR[free_index].ref_cnt = 0;

        return 0;
}

int fs_delete(char* name) {
        // check if file exists
        int file_index = -1;
        int i;
        for (i=0; i<MAX_FILES; i++) {
                if (DIR[i].used && strcmp(DIR[i].name, name) == 0) {
                        file_index = i;
                        break;
                }
        }

        if (file_index == -1) {
                perror("ERROR: fs_delete file not found.");
                return -1;
        }

        // check if file is open
        if (DIR[file_index].ref_cnt > 0) {
                return -1; // file is open
        }

        // free data blocks
        int block = DIR[file_index].head;
        while (block != FREE) {
                int next_block = FAT[block];
                FAT[block] = FREE;
                block = next_block;
        }

        // remove file from directory
        DIR[file_index].used = 0;
        DIR[file_index].size = 0;
        DIR[file_index].head = FREE;
        DIR[file_index].ref_cnt = 0;
        DIR[file_index].name[0] = '\0';

        return 0;
}

// helper function for write
int allocate_block() {
        int i;
        for (i=0; i<FAT_SIZE; i++) {
                if (FAT[i] == FREE) {
                        FAT[i] = -2; // mark block as in use
                        return i;
                }
        }

        return -1; // no free block available
}



int fs_read(int fildes, void* buf, size_t nbyte) {
        // validate file descriptor
        if (fildes < 0 || fildes >= MAX_FILDES || !global_fd_array[fildes].used) {
                return -1; // invalid file descriptor
        }

        // retrieve file descriptor and file info
        //struct file_descriptor* fd = &global_fd_array[fildes];
        int index = global_fd_array[fildes].file;
        struct dir_entry* file = &DIR[index];

        if (global_fd_array[fildes].offset >= file->size || file->size == 0) {
                return 0; // offset is beyond end of file or file is empty
        }

        size_t toRead = nbyte;
        if (global_fd_array[fildes].offset + nbyte > file->size) {
                toRead = file->size - global_fd_array[fildes].offset;
        }

        size_t bytesRead = 0;
        int blockIndex = file->head;
        int offsetInFile = global_fd_array[fildes].offset;

        while (toRead>0 && blockIndex != 1 && blockIndex != -2) {
                char block[BLOCK_SIZE];
                if (block_read(blockIndex, block) < 0) {
                        return -1;
                }
                int offsetWithinBlock = offsetInFile % BLOCK_SIZE;
                int availableInBlock = BLOCK_SIZE - offsetWithinBlock;
                int copyAmount;
                if (toRead < availableInBlock) {
                        copyAmount = toRead;
                } else {
                        copyAmount = availableInBlock;
                }

                memcpy((char*)buf + bytesRead, block + offsetWithinBlock, copyAmount);

                bytesRead += copyAmount;
                toRead -= copyAmount;
                offsetInFile += copyAmount;
                blockIndex = FAT[blockIndex];
        }

        global_fd_array[fildes].offset += bytesRead;
        return bytesRead;
}

int fs_write(int fildes, void* buf, size_t nbyte) {
        // validate file descriptor
        if (fildes<0 || fildes>=MAX_FILDES || !global_fd_array[fildes].used) {
                perror("ERROR: fs_write invalid file descriptor.");
                return -1;
        }

        // retrieve file descriptor and file into
        struct file_descriptor* fd = &global_fd_array[fildes];
        struct dir_entry* file = &DIR[fd->file];

        // prepare for writing data
        int total_bytes_written = 0;
        const char* buffer = (const char*)buf;

        while (total_bytes_written < nbyte) {
                int current_block = file->head;
                int block_offset = fd->offset / BLOCK_SIZE;
                int block_write_offset = fd->offset % BLOCK_SIZE;
                int prev_block = FREE;

                // find or allocate correct block to write to
                int i;
                for (i=0; i<block_offset; i++) {
                        if (current_block == FREE) {
                                // allocate a new block if necessary
                                current_block = allocate_block();
                                if (current_block == -1) {
                                        break; // no more blocks available
                                }
                                // update FAT and file head if it's first block
                                if (i==0) {
                                        file->head = current_block;
                                } else {
                                        FAT[prev_block] = current_block;
                                }
                        }
                        prev_block = current_block;
                        current_block = FAT[current_block];
                }


                // case where current block is still -1
                if (current_block == FREE) {
                        current_block = allocate_block();
                        if (current_block == -1) {
                                break; // no more block available
                        }
                        if (file->head == FREE) {
                                file->head = current_block; // update file if it's new
                        } else {
                                FAT[current_block] = current_block; // update FAT for pre block
                        }
                }

                // write data to block
                char block_data[BLOCK_SIZE];
                if (block_read(current_block, block_data) == -1) {
                        return -1; // error reading block

                }

                int bytes_to_write = min(nbyte - total_bytes_written, BLOCK_SIZE - block_write_offset);
                memcpy(block_data + block_write_offset, buffer + total_bytes_written, bytes_to_write);

                if (block_write(current_block, block_data) == -1) {
                        return -1; // error writing block
                }

                // update counters
                total_bytes_written += bytes_to_write;
                fd->offset += bytes_to_write;
                file->size = max(file->size, fd->offset);
        }

        // update file descriptor with new offset
        global_fd_array[fildes].offset = fd->offset;

        return total_bytes_written;
}

int fs_get_filesize(int fildes) {
        // validate file descriptor
        if (fildes<0 || fildes>=MAX_FILDES || !global_fd_array[fildes].used) {
                perror("ERROR: fs_get_filesize invalid file descriptor.");
                return -1;
        }

        // retrieve file size from directory entry
        int file_index = global_fd_array[fildes].file;
        if (file_index<0 || file_index>=MAX_FILES || !DIR[file_index].used) {
                return -1; // file not found or deleted
        }

        return DIR[file_index].size;
}

int fs_listfiles(char*** files) {
        // count number of files
        int num_files = 0;
        int i;
        for (i=0; i<MAX_FILES; i++) {
                if (DIR[i].used) {
                        num_files++;
                }
        }

        // allocate memory for array of file names
        *files = (char**)malloc((num_files+1) * sizeof(char*)); // includes NULL terminator
        if (*files == NULL) {
                perror("ERROR: fs_listfiles failed to allocate memory.");
                return -1;
        }

        // populate array with file names
        int j=0;
        for (i=0; i<MAX_FILES; i++) {
                if (DIR[i].used) {
                        (*files)[j] = strdup(DIR[i].name);
                        if ((*files)[j] == NULL) {
                                int k;
                                for (k=0; k<j; k++) {
                                        free((*files)[k]);
                                }
                                free(*files);
                                perror("ERROR: fs_listfiles failed to allocate memory for file name.");
                                return -1;
                        }
                        j++;
                }
        }

        // terminate array with NULL pointer
        (*files)[num_files] = NULL;

        return 0;
}

int fs_lseek(int fildes, off_t offset) {
        // validate file descriptor
        if (fildes<0 || fildes>=MAX_FILDES ||!global_fd_array[fildes].used) {
                perror("ERROR: fs_lseek invalid file descriptor.");
                return -1;
        }

        // retrieve file descriptor and file into
        struct file_descriptor* fd = &global_fd_array[fildes];
        struct dir_entry* file_entry = &DIR[fd->file];

        if (offset < 0 || offset > file_entry->size) {
                return -1; // offset out of bounds
        }

        // set file offset
        fd->offset = offset;

        return 0;
}

int fs_truncate(int fildes, off_t length) {
        // validate file descriptor
        if (fildes<0 || fildes>= MAX_FILDES || !global_fd_array[fildes].used) {
                perror("ERROR: fs_truncate invalid file descriptor.");
                return -1;
        }

        // retrieve file desccriptor and file info
        struct file_descriptor* fd = &global_fd_array[fildes];
        struct dir_entry* file = &DIR[fd->file];

        // check requested length       
        if (length < 0 || file->size) {
                perror("ERROR: fs_truncate length larger than file size.");
                return -1;
        }

        int current_block = file->head;
        int block_count = length / BLOCK_SIZE;
        int last_block = block_count + (length % BLOCK_SIZE != 0); // last block to keep

        int i;
        for (i=0; i<last_block; ++i) {
                if (i < block_count) {
                        current_block = FAT[current_block];
                } else {
                        // free remaining block 
                        while (current_block != FREE) {
                                int next_block = FAT[current_block];
                                FAT[current_block] = FREE;
                                current_block = next_block;
                        }
                }
        }

        // update file size and possibly file pointer
        file->size = length;
        if (fd->offset > length) {
                fd->offset = length;
        }

        return 0;
}
