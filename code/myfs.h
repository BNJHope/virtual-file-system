#include "fs.h"
#include <stdio.h>
#include <linux/limits.h>
#include <sys/stat.h>

#define MY_MAX_FILENAME FILENAME_MAX
#define MY_MAX_PATH PATH_MAX
#define MY_MAX_FILE_SIZE 1000

typedef struct {

	//the path to this file
	char *path;

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

//key value map for a directory entry
typedef struct {

	//the path to the file that this
	char *path;

	//the uuid of the file_node so that it can be found in the database
	uuid_t fileNodeId;

} dir_entry;

//collection of key value maps for a directory entry
typedef struct {

	//array of pointers to directory entries
	dir_entry **entries;

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
int getFileNode(const char* path, file_node* fnode);