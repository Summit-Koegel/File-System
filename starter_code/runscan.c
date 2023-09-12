#include "ext2_fs.h"
#include "read_ext2.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#define INDIRECT_BLK_CNT (block_size / sizeof(uint32_t))

struct ext2_super_block super;
struct ext2_group_desc group;

int is_jpg(char* buffer) {
    if (buffer[0] == (char)0xff &&
        buffer[1] == (char)0xd8 &&
        buffer[2] == (char)0xff &&
        (buffer[3] == (char)0xe0 ||
        buffer[3] == (char)0xe1 ||
        buffer[3] == (char)0xe8)) {
    return 1;
    }
    return 0;
}

void read_dir(int fd, struct ext2_inode *inode, char * buffer){

    lseek(fd, BLOCK_OFFSET(inode->i_block[0]), SEEK_SET);
    read(fd, buffer, 1024);

    uint32_t offset = 0;
    while(offset < inode->i_size) {
        struct ext2_dir_entry_2 * dentry;
        dentry = (struct ext2_dir_entry_2*) & ( buffer[offset] );
        if (dentry->rec_len == 0 || dentry->name_len == 0){
            break;
        }
        int name_len = dentry->name_len & 0xFF; // convert 2 bytes to 4 bytes properly
        char name [EXT2_NAME_LEN];
        strncpy(name, dentry->name, name_len);
        name[name_len] = '\0';
        printf("Entry name is --%s--", name);
        
        //offset += 8 + dentry->name_len; ???
        offset = ((offset + 4 - 1) / 4) * 4;
    }
}

void handle_indirect_blocks(int fd, uint32_t block_num, char *buf, FILE *output_file, uint32_t block_size, uint32_t *bytes_left, int indirection_level){

    // exit for recursion
    if (indirection_level < 1 || indirection_level > 3 ||  *bytes_left == 0 || block_num == 0){
        return;
    }

    off_t offset = BLOCK_OFFSET(block_num);
    uint32_t indirect_block[INDIRECT_BLK_CNT];
    lseek(fd, offset, SEEK_SET);
    read(fd, indirect_block, block_size);

    for (uint32_t i = 0; i < INDIRECT_BLK_CNT && *bytes_left > 0; i++){
        if (indirect_block[i] == 0){
            continue;
        }
        if (indirection_level == 1){
            // only read bytes left if left than block size
            uint32_t bytes_to_read = (*bytes_left < block_size) ? *bytes_left : block_size;
            offset = BLOCK_OFFSET(indirect_block[i]);
            lseek(fd, offset, SEEK_SET);
            read(fd, buf, bytes_to_read);
            fwrite(buf, 1, bytes_to_read, output_file);
            *bytes_left -= bytes_to_read;
        }
        else{
            // recurse further down indirection chain
            handle_indirect_blocks(fd, indirect_block[i], buf, output_file, block_size, bytes_left, indirection_level - 1);
        }
    }
}


void process_dir_blks(int fd, uint32_t block_num, uint32_t block_size, uint32_t inode_num, char *file_name, int *done, int indirection_level){
    
    // initial check for recursion
    if (block_num == 0 || *done || indirection_level < 0 ){
        return;
    }

    // if we found an indirect level, unpack and recursivel call with new args
    if (indirection_level){
        uint32_t indirect_block[INDIRECT_BLK_CNT];
        off_t offset = BLOCK_OFFSET(block_num);
        lseek(fd, offset, SEEK_SET);
        read(fd, indirect_block, block_size);

        // Process all block counts
        for (uint32_t i = 0; i < INDIRECT_BLK_CNT && !*done; i++){
            process_dir_blks(fd, indirect_block[i], block_size, inode_num, file_name, done, indirection_level - 1);
        }
    }
    else { // if block is not indirect
        char buf[block_size];
        off_t offset = BLOCK_OFFSET(block_num);
        lseek(fd, offset, SEEK_SET);
        read(fd, buf, block_size);

        struct ext2_dir_entry_2 *entry = (struct ext2_dir_entry_2 *)buf;
        while ((char *)entry < buf + block_size && !*done){
            // find inode
            if (entry->inode == inode_num){
                strncpy(file_name, entry->name, entry->name_len);
                file_name[entry->name_len] = '\0';
                *done = 1;
                break;
            }
            // increment offset
            off_t off = 8 + entry->name_len;
            if (off % 4 != 0){
                off = off + 4 - (off % 4);
            }
            entry = (struct ext2_dir_entry_2 *)((char *)entry + off); // for next iteration
        }
    }
}


int get_filename(int fd, struct ext2_super_block *super, struct ext2_group_desc *group, char *filename, uint32_t inode_num){
    uint32_t inodes_p_g = super->s_inodes_per_group;
    uint32_t block_sz = 1024;
    struct ext2_inode dir_inode;

    // get groups like read_ext2.c
    uint32_t num_groups = (super->s_blocks_count + super->s_blocks_per_group - 1) / super->s_blocks_per_group;

    int done = 0;
    // iterate though all groups and find filename
    for (uint32_t j = 0; j < num_groups && !done; j++){
        off_t inode_table_offset = locate_inode_table(j, group);

        for (uint32_t i = 1; i < inodes_p_g && !done; i++){
            read_inode(fd, inode_table_offset, i, &dir_inode, super->s_inode_size);
            if (S_ISDIR(dir_inode.i_mode)){
                process_dir_blks(fd, dir_inode.i_block[0], block_sz, inode_num, filename, &done, 0);
            }
        }
    }
    return done;
}


int main(int argc, char **argv) 
{
    if (argc != 3) 
    {
        printf("expected usage: ./runscan inputfile outputfile\n");
        exit(0);
    }

    /* This is some boilerplate code to help you get started, feel free to modify
       as needed! */

    int fd;
    fd = open(argv[1], O_RDONLY);    /* open disk image */

    DIR *dir = opendir(argv[2]);
    if(dir != NULL){
        printf("Err: directory already exists.\n"); 
        closedir(dir);
        exit(1);
    }

    struct stat st = {0};
    if (stat(argv[2], &st) == -1){
        if(mkdir(argv[2], 0700) == -1){
            perror("mkdir");
            exit(1);
        }
    }

    ext2_read_init(fd);
    read_super_block(fd, 0, &super);
    uint32_t inodes_p_g = super.s_inodes_per_group;
    uint32_t inode_size = super.s_inode_size;
    uint32_t num_groups = (super.s_blocks_count + super.s_blocks_per_group - 1) / super.s_blocks_per_group;
    /////////////////////////////////////

    for(uint32_t i = 0; i < num_groups; i++){

        read_super_block(fd, i, &super);
        read_group_desc(fd, i, &group);

        off_t table_offset = locate_inode_table(i, &group);

        uint32_t start_of_group = i * inodes_p_g;

        for (uint32_t a = 0; a < inodes_p_g; a++) {

            uint32_t current_inode_num = start_of_group + a;

            char buffer[1024];
            struct ext2_inode inode;

            read_inode(fd, table_offset, a, &inode, inode_size);

            if(S_ISREG(inode.i_mode)){
                off_t offset = BLOCK_OFFSET(inode.i_block[0]);
                lseek(fd, offset, SEEK_SET);
                read(fd, buffer, block_size);

                if (is_jpg(buffer)){
                    /*********Write details and filename ********************/
                    char output_dir[256];
                    char file_name[256];

                    snprintf(output_dir, sizeof(output_dir), "%s/file-%u.jpg", argv[2], current_inode_num);
                    get_filename(fd,&super,&group,file_name,current_inode_num);
                    char output_text_path[256];
                    snprintf(output_text_path, sizeof(output_text_path), "%s/file-%u-details.txt", argv[2], current_inode_num);
                    
                    FILE *file = fopen(output_text_path, "w");
                    if (file == NULL){
                        printf("Failed to open the file for writing.\n");
                        return 1;
                    }
                    // write info for details to output details file
                    fprintf(file, "%d\n%d\n%d", inode.i_links_count, inode.i_size, inode.i_uid);
                    fclose(file);
                
                    // print out filename
                    char output_path2[256];
                    snprintf(output_path2, sizeof(output_path2) + 1, "%s/%s", argv[2], file_name);
                    /*********************************************/
                    
                    FILE *output_file = fopen(output_dir, "wb");

                    if (!output_file){
                        perror("Error opening output file");
                        exit(1);
                    }
                    uint32_t bytes_left = inode.i_size;
                    uint32_t bytes_to_read = block_size;

                    // if 0 indirection levels
                    for (int block_num = 0; block_num < EXT2_NDIR_BLOCKS; block_num++){
                        if (bytes_left == 0){
                            break;
                        }
                        if (bytes_left < block_size){
                            bytes_to_read = bytes_left;
                        }
                        offset = BLOCK_OFFSET(inode.i_block[block_num]);
                        lseek(fd, offset, SEEK_SET);
                        read(fd, buffer, bytes_to_read);
                        fwrite(buffer, 1, bytes_to_read, output_file);
                        bytes_left -= bytes_to_read;
                    }

                    // if there are indirection levels
                    for (int indirection_level = 1; indirection_level <= 3 && bytes_left > 0; indirection_level++){
                        int block_num = EXT2_IND_BLOCK + (indirection_level - 1);
                        if (inode.i_block[block_num] != 0){
                            handle_indirect_blocks(fd, inode.i_block[block_num], buffer, output_file, block_size, &bytes_left, indirection_level);
                        }
                    }
                    fclose(output_file);
                    FILE *number_file = fopen(output_dir, "rb");
                    FILE *name_file = fopen(output_path2, "wb");

                    // copy all info over 
                    int single_char;
                    while (1){
                        single_char = fgetc(number_file);
                        if (single_char == EOF)
                            break;
                        fputc(single_char, name_file);
                    }
                    fclose(name_file);
                    fclose(number_file);
                }
            }
        }

    }
    closedir(dir);
    close(fd);
    return 0;
}