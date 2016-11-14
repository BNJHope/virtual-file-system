#include "fs.h"
#include <stdio.h>
#include <linux/limits.h>
#include <sys/stat.h>

//set the maximum length of a file name as the system limit
#define MY_MAX_FILENAME FILENAME_MAX

//set the maximum length of a path as the system limit
#define MY_MAX_PATH PATH_MAX

//set the maximum size of a directory
#define MY_MAX_DIRECTORY_SIZE 256

//set the number of memory blocks that each indirect block holds
//and the number of indirect blocks the overall regular data
//data structure will hold
#define MY_MAX_BLOCK_LIMIT 12

//the number of bytes that are being stored in every file block
#define MY_BLOCK_SIZE 8

//defines the size in bytes of a single indirect block
#define MY_SINGLE_INDIRECT_BLOCK_SIZE (MY_MAX_BLOCK_LIMIT * MY_BLOCK_SIZE)

//define the size in bytes of the collection of direct blocks kept in a
//regular file data structure
#define MY_DIRECT_BLOCKS_SIZE (MY_BLOCK_SIZE * MY_MAX_BLOCK_LIMIT)

//define the size in bytes of all of the indirect blocks
#define MY_INDIRECT_BLOCKS_SIZE (MY_MAX_BLOCK_LIMIT * MY_SINGLE_INDIRECT_BLOCK_SIZE)

//define the maximum size of a file as the size of a collection of indirect
//data blocks plus the size of the collection of indirect data blocks
#define MY_MAX_FILE_SIZE (MY_DIRECT_BLOCKS_SIZE + MY_INDIRECT_BLOCKS_SIZE)

//true and false values
#define TRUE 1
#define FALSE 0

//takes a mode from a file and returns whether or not this file is a directory
#define IS_DIR(mode) (!(S_ISREG(mode)))

//the file name for the root directory
#define ROOT_NAME "/"

//takes a file path and determines whether the file is the root directory or not
#define IS_ROOT(path) ((strcmp(path, ROOT_NAME)) == 0)

//takes a uuid_t and determines whether or not it is the zero_uuid, which would
//mean that a data structure has not yet been allocated a uuid_t
#define UUID_IS_BLANK(uuidToCheck) (uuid_compare(zero_uuid, uuidToCheck) == 0)

//the file control block used in my file system
typedef struct {

	//the file name
	char path[MY_MAX_FILENAME + 1];

	//the uuid for the location of where the data of this node is located
	//in the database
	uuid_t data_id;

	//mode of the file
	mode_t mode;

	//user ID
	uid_t uid;

	//group ID
	gid_t gid;

	//the time of the last modification
	time_t mtime;

	//the time of the last change to the meta-data
	time_t ctime;

	//the time of the last access to the file
	time_t atime;

	//size of the file
	off_t size;

} file_node;

//a single data block
typedef struct {

	//collection of bytes to read.
	uint8_t data[MY_BLOCK_SIZE];

} data_block;

//a collection of data blocks used for single indirect
//nodes
typedef struct {

	//array of data blocks
	uuid_t data_blocks[MY_MAX_BLOCK_LIMIT];

} single_indirect;

//data structure for a regular file data
typedef struct {

	//array of direct file blocks
	uuid_t direct_blocks[MY_MAX_BLOCK_LIMIT];

	//array of indirect blocks for the 
	uuid_t indirect_blocks[MY_MAX_BLOCK_LIMIT];

} reg_data;

//a key value map for mapping the filename of a single file
//kept in a directory to the uuid_t of its file control block
typedef struct {

	//the path to the file that this
	char path[MY_MAX_FILENAME];

	//the uuid of the file_node so that it can be found in the database
	uuid_t fileNodeId;

} dir_entry;

//collection of key value maps that the directory holds
typedef struct {

	//array of directory entries
	dir_entry entries[MY_MAX_DIRECTORY_SIZE];

} dir_data;

//the old file control block
typedef struct myfcb{
	char path[MY_MAX_PATH];
	uuid_t file_data_id;

	// see 'man 2 stat' and 'man 2 chmod'
	//meta-data for the 'file'
	uid_t  uid;		/* user */
    gid_t  gid;		/* group */
	mode_t mode;	/* protection */
	time_t mtime;	/* time of last modification */
	time_t ctime;	/* time of last change to meta-data (status) */
	off_t size;		/* size */

	//meta-data for the root thing (directory)
	uid_t  root_uid;		/* user */
    gid_t  root_gid;		/* group */
	mode_t root_mode;	/* protection */
	time_t root_mtime;	/* time of last modification */
} MyFCB;

//fetch the file control block at the given data id and copy it into
//the buffer passed to the block
int fetchFCBFromUnqliteStore(uuid_t *data_id, file_node *buffer);

//store the file control block given in the buffer into the database
//store at the given key
int storeFCBInUnqliteStore(uuid_t *key_id, file_node *value_addr);

//fetch a directory data structure from the database at the given key and load
//it into the given buffer
int fetchDirectoryDataFromUnqliteStore(uuid_t *data_id, dir_data *buffer);

//store the directory data structure given at the given key
int storeDirectoryDataFromUnqliteStore(uuid_t *key_id, dir_data *value_addr);

//fetch a regular file structure from the database at the given key and load
//it into the given buffer
int fetchRegularFileDataFromUnqliteStore(uuid_t *data_id, reg_data *buffer);

//store the regular file structure structure given at the given key
int storeRegularFileDataFromUnqliteStore(uuid_t *key_id, reg_data *value_addr);

//fetch a single indirect block structure from the database at the given key and load
//it into the given buffer
int fetchSingleIndirectBlockFromUnqliteStore(uuid_t *data_id, single_indirect *buffer);

//store the single indirect structure structure given at the given key
int storeSingleIndirectBlockFromUnqliteStore(uuid_t *key_id, single_indirect *value_addr);

//fetch a single data block structure from the database at the given key and load
//it into the given buffer
int fetchDataBlockFromUnqliteStore(uuid_t *data_id, data_block *buffer);

//store the single data block structure given at the given key
int storeDataBlockFromUnqliteStore(uuid_t *key_id, data_block *value_addr);

//update the root object
int updateRootObject();

//fill the given stat struct with the data from the file control block
int fillStatWithFileNode(struct stat* destination, file_node* source);

//create a new file control block for the file at the given path by filling
//the file control block buffer
int initNewFCB(const char* path, file_node* buff);

//retrieve the file control block of the file at the given path and also
//the uuid of where that file control block is being kept in the database
int getFileNode(const char* path, file_node* fnode, uuid_t* fnode_uuid);

//retrieve the file control block of the parent directory of the file
//at the given path and also the uuid of where that file control block
//is being kept in the database
int getParentFileNode(const char* path, file_node *buffer, uuid_t* buff_uuid);

//create a direct entry to link the file name with the given parent node and return it
//in the directory entry buffer and store the directory entry in the given directory data
int makeDirent(char* filename, file_node *parentNode, dir_entry *dirent, dir_data *dirdata);

int makeCWDdirent(uuid_t *cwdId, dir_data *newDirData);
int makePDdirent(uuid_t *pdId, dir_data *newDirData);

//get the offsets needed for the indirect block, direct block and interior character in the data buffer from
//the offset value that has been given
int getOffsets(off_t offset, int *indir_block_offset, int *small_block_offset, int *interior_block_offset);

//get the size of a memory block to read depending on the offsets and the remaining size to be read
int getSizeToRead(size_t *size_to_read, size_t *remaining_size, int *interior_block_offset);

//reads a data block at the given source uuid and copies it into the destination uuid. If
//it does not exist then we return -1 so that the read function knows to return the
//number of bytes that have been read at this point since there is nothing left
int getReadDataBlockData(data_block *curr_block, uuid_t *dest, uuid_t *src);

//reads a data block at the given source uuid and copies it into the destination uuid. If
//it does not exist then we generate a new uuid for the block
int getWriteDataBlockData(data_block *curr_block, uuid_t *dest, uuid_t *src);

//reads a single indirect block at the given source uuid and copies it into the destination uuid. If
//it does not exist then we return -1 so that the read function knows to return the
//number of bytes that have been read at this point since there is nothing left
int getReadSingleIndirectBlock(single_indirect *indir_block, uuid_t *dest, uuid_t *src);

//reads a single indirect block at the given source uuid and copies it into the destination uuid. If
//it does not exist then we generate a new uuid for the block
int getWriteSingleIndirectBlock(single_indirect *indir_block, uuid_t *dest, uuid_t *src);