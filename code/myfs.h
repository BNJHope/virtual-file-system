#include "fs.h"
#include <stdio.h>
#include <linux/limits.h>
#include <sys/stat.h>

#define MY_MAX_FILENAME FILENAME_MAX
#define MY_MAX_PATH PATH_MAX
#define MY_MAX_FILE_SIZE 1000
#define MY_MAX_DIRECTORY_SIZE 256
#define MY_MAX_BLOCK_LIMIT 12
#define MY_BLOCK_SIZE 64
#define TRUE 1
#define FALSE 0
#define IS_DIR(mode) (!(S_ISREG(mode)))
#define ROOT_NAME "/"
#define IS_ROOT(path) ((strcmp(path, ROOT_NAME)) == 0)

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

typedef struct {

	//collection of bytes to read.
	char data[MY_BLOCK_SIZE];

} data_block;

//a colleciton of data blocks used
typedef struct {

	//array of data blocks
	data_block data_blocks[MY_MAX_BLOCK_LIMIT];

} data_collection;

//data structure for a regular file data
typedef struct {

	//array of direct file blocks
	data_block direct_blocks[MY_MAX_BLOCK_LIMIT];

	//array of indirect blocks for the 
	data_collection indirect_blocks[MY_MAX_BLOCK_LIMIT];

} reg_data;

//key value map for a directory entry
typedef struct {

	//the path to the file that this
	char path[MY_MAX_FILENAME];

	//the uuid of the file_node so that it can be found in the database
	uuid_t fileNodeId;

} dir_entry;

//collection of key value maps for a directory entry
typedef struct {

	//array of directory entries
	dir_entry entries[MY_MAX_DIRECTORY_SIZE];

} dir_data;

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

int fetchFCBFromUnqliteStore(uuid_t *data_id, file_node *buffer);
int storeFCBInUnqliteStore(uuid_t *key_id, file_node *value_addr);
int fetchDirectoryDataFromUnqliteStore(uuid_t *data_id, dir_data *buffer);
int storeDirectoryDataFromUnqliteStore(uuid_t *key_id, dir_data *value_addr);
int updateRootObject();
int fillStatWithFileNode(struct stat* destination, file_node* source);
int getFileNode(const char* path, file_node* fnode, uuid_t* fnode_uuid);
int initNewFCB(const char* path, file_node* buff);
int getParentFileNode(const char* path, file_node *buffer, uuid_t* buff_uuid);
int makeDirent(char* filename, file_node *parentNode, dir_entry *dirent, dir_data *dirdata);
int makeCWDdirent(uuid_t *cwdId, dir_data *newDirData);
int makePDdirent(uuid_t *pdId, dir_data *newDirData);