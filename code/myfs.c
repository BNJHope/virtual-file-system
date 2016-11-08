/*
  MyFS. One directory, one file, 1000 bytes of storage. What more do you need?

  This Fuse file system is based largely on the HelloWorld example by Miklos Szeredi <miklos@szeredi.hu> (http://fuse.sourceforge.net/helloworld.html). Additional inspiration was taken from Joseph J. Pfeiffer's "Writing a FUSE Filesystem: a Tutorial" (http://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/).
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "myfs.h"
#include <libgen.h>

// The one and only fcb that this implmentation will have. We'll keep it in memory. A better
// implementation would, at the very least, cache it's root directroy in memory.
file_node the_root_fcb;

//String representations of the current working directory and the parent directory
char *CWD_STR = ".", *PD_STR = "..";

// Get file and directory attributes (meta-data).
// Read 'man 2 stat' and 'man 2 chmod'.
static int myfs_getattr(const char *path, struct stat *stbuf){
	write_log("myfs_getattr(path=\"%s\", statbuf=0x%08x)\n", path, stbuf);

	//gets the file_node struct if it exists
	read_root();
	file_node fnode = the_root_fcb;

	// if(getFileNode(path, &fnode) == -1) {
	// 	printf("myfs_getattr - file not found");
	// 	write_log("myfs_getattr - file not found");
	// 	return -1;
	// }

	//clear enough space for a stat by setting a stat's worth of space
	//all to 0 so that it can be used for a new stat struct
	memset(stbuf, 0, sizeof(struct stat));

	fillStatWithFileNode(stbuf, &fnode);

	// if(strcmp(path, "/")==0){
	// 	stbuf->st_mode = the_root_fcb.mode;
	// 	stbuf->st_nlink = 2;
	// 	stbuf->st_uid = the_root_fcb.uid;
	// 	stbuf->st_gid = the_root_fcb.gid;
	// } else {
	// 	if (strcmp(path, the_root_fcb.path) == 0) {
	// 		stbuf->st_mode = the_root_fcb.mode;
	// 		stbuf->st_nlink = 1;
	// 		stbuf->st_mtime = the_root_fcb.mtime;
	// 		stbuf->st_ctime = the_root_fcb.ctime;
	// 		stbuf->st_size = the_root_fcb.size;
	// 		stbuf->st_uid = the_root_fcb.uid;
	// 		stbuf->st_gid = the_root_fcb.gid;
	// 	}else{
	// 		write_log("myfs_getattr - ENOENT");
	// 		return -ENOENT;
	// 	}
	// }

	return 0;
}

// Read a directory.
// Read 'man 2 readdir'.
static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi){
	(void) offset;
	(void) fi;

	write_log("write_readdir(path=\"%s\", buf=0x%08x, filler=0x%08x, offset=%lld, fi=0x%08x)\n", path, buf, filler, offset, fi);

	file_node fnode;
	
	/*
	if(getFileNode(path, &fnode) == -1) {
		printf("myfs_getattr - file not found");
		write_log("myfs_getattr - file not found");
		return -1;
	}
	*/

	struct stat cwdStat;
	stat(the_root_fcb.path, &cwdStat);

	filler(buf, ".", NULL, 0);
	//filler(buf, "..", NULL, 0);

	char *pathP = (char*)&(the_root_fcb.path);
	if(*pathP!='\0'){
		// drop the leading '/';
		pathP++;
		filler(buf, pathP, NULL, 0);
	}

	return 0;
}

// Read a file.
// Read 'man 2 read'.
static int myfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	size_t len;
	(void) fi;

	write_log("myfs_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n", path, buf, size, offset, fi);

	if(strcmp(path, the_root_fcb.path) != 0){
		write_log("myfs_read - ENOENT");
		return -ENOENT;
	}

	len = the_root_fcb.size;

	uint8_t data_block[MY_MAX_FILE_SIZE];

	memset(&data_block, 0, MY_MAX_FILE_SIZE);
	uuid_t *data_id = &(the_root_fcb.data_id);
	// Is there a data block?
	if(uuid_compare(zero_uuid,*data_id)!=0){
		unqlite_int64 nBytes;  //Data length.
		int rc = unqlite_kv_fetch(pDb,data_id,KEY_SIZE,NULL,&nBytes);
		if( rc != UNQLITE_OK ){
		  error_handler(rc);
		}
		if(nBytes!=MY_MAX_FILE_SIZE){
			write_log("myfs_read - EIO");
			return -EIO;
		}

		// Fetch the fcb the root data block from the store.
		unqlite_kv_fetch(pDb,data_id,KEY_SIZE,&data_block,&nBytes);
	}

	if (offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, &data_block + offset, size);
	} else
		size = 0;

	return size;
}

// This file system only supports one file. Create should fail if a file has been created. Path must be '/<something>'.
// Read 'man 2 creat'.
static int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi){
    write_log("myfs_create(path=\"%s\", mode=0%03o, fi=0x%08x)\n", path, mode, fi);

    //file_node space for the root node to start and the new file block
    file_node parent, newFileBlock;

    //fetch the root object from the database
    fetchFCBFromUnqliteStore(&root_object.id, &parent);
    printf("path : %s\n", path);

    //initialise a new file block for the new file
    initNewFCB(path, &newFileBlock);

 	//    if(the_root_fcb.path[0] != '\0'){
	// 	write_log("myfs_create - ENOSPC");
	// 	return -ENOSPC;
	// }

	int pathlen = strlen(path);
	if(pathlen>=MY_MAX_PATH){
		write_log("myfs_create - ENAMETOOLONG");
		return -ENAMETOOLONG;
	}

	dir_entry dirent;
	sprintf(dirent.path, path);
	uuid_copy(newFileBlock.data_id, dirent.fileNodeId);

	dir_data dirdata;


	printf("%s\n", dirent.path);
	struct fuse_context *context = fuse_get_context();

	storeFCBInUnqliteStore(&dirent.fileNodeId, &newFileBlock);
	int rc = unqlite_kv_store(pDb,&(root_object.id),KEY_SIZE,&the_root_fcb,sizeof(file_node));
	if( rc != UNQLITE_OK ){
		write_log("myfs_create - EIO");
		return -EIO;
	}

    return 0;
}

// Set update the times (actime, modtime) for a file. This FS only supports modtime.
// Read 'man 2 utime'.
static int myfs_utime(const char *path, struct utimbuf *ubuf){
    write_log("myfs_utime(path=\"%s\", ubuf=0x%08x)\n", path, ubuf);

	if(strcmp(path, the_root_fcb.path) != 0){
		write_log("myfs_utime - ENOENT");
		return -ENOENT;
	}
	the_root_fcb.mtime=ubuf->modtime;

	// Write the fcb to the store.
    int rc = unqlite_kv_store(pDb,&(root_object.id),KEY_SIZE,&the_root_fcb,sizeof(file_node));
	if( rc != UNQLITE_OK ){
		write_log("myfs_write - EIO");
		return -EIO;
	}

    return 0;
}

// Write to a file.
// Read 'man 2 write'
static int myfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
    write_log("myfs_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n", path, buf, size, offset, fi);

	if(strcmp(path, the_root_fcb.path) != 0){
		write_log("myfs_write - ENOENT");
		return -ENOENT;
    }

	if(size >= MY_MAX_FILE_SIZE){
		write_log("myfs_write - EFBIG");
		return -EFBIG;
	}

	uint8_t data_block[MY_MAX_FILE_SIZE];

	memset(&data_block, 0, MY_MAX_FILE_SIZE);
	uuid_t *data_id = &(the_root_fcb.data_id);
	// Is there a data block?
	if(uuid_compare(zero_uuid,*data_id)==0){
		// GEnerate a UUID fo rhte data blocl. We'll write the block itself later.
		uuid_generate(the_root_fcb.data_id);
	}else{
		// First we will check the size of the obejct in the store to ensure that we won't overflow the buffer.
		unqlite_int64 nBytes;  // Data length.
		int rc = unqlite_kv_fetch(pDb,data_id,KEY_SIZE,NULL,&nBytes);
		if( rc!=UNQLITE_OK || nBytes!=MY_MAX_FILE_SIZE){
			write_log("myfs_write - EIO");
			return -EIO;
		}

		// Fetch the data block from the store.
		unqlite_kv_fetch(pDb,data_id,KEY_SIZE,&data_block,&nBytes);
		// Error handling?
	}

	// Write the data in-memory.
    int written = snprintf(data_block, MY_MAX_FILE_SIZE, buf);

	// Write the data block to the store.
	int rc = unqlite_kv_store(pDb,data_id,KEY_SIZE,&data_block,MY_MAX_FILE_SIZE);
	if( rc != UNQLITE_OK ){
		write_log("myfs_write - EIO");
		return -EIO;
	}

	// Update the fcb in-memory.
	the_root_fcb.size=written;
	time_t now = time(NULL);
	the_root_fcb.mtime=now;
	the_root_fcb.ctime=now;

	// Write the fcb to the store.
    rc = unqlite_kv_store(pDb,&(root_object.id),KEY_SIZE,&the_root_fcb,sizeof(file_node));
	if( rc != UNQLITE_OK ){
		write_log("myfs_write - EIO");
		return -EIO;
	}

    return written;
}

// Set the size of a file.
// Read 'man 2 truncate'.
int myfs_truncate(const char *path, off_t newsize){
    write_log("myfs_truncate(path=\"%s\", newsize=%lld)\n", path, newsize);

	if(newsize >= MY_MAX_FILE_SIZE){
		write_log("myfs_truncate - EFBIG");
		return -EFBIG;
	}

	the_root_fcb.size = newsize;

	// Write the fcb to the store.
    int rc = unqlite_kv_store(pDb,&(root_object.id),KEY_SIZE,&the_root_fcb,sizeof(file_node));
	if( rc != UNQLITE_OK ){
		write_log("myfs_write - EIO");
		return -EIO;
	}

	return 0;
}

// Set permissions.
// Read 'man 2 chmod'.
int myfs_chmod(const char *path, mode_t mode){
    write_log("myfs_chmod(fpath=\"%s\", mode=0%03o)\n", path, mode);

    return 0;
}

// Set ownership.
// Read 'man 2 chown'.
int myfs_chown(const char *path, uid_t uid, gid_t gid){
    write_log("myfs_chown(path=\"%s\", uid=%d, gid=%d)\n", path, uid, gid);

    return 0;
}

// Create a directory.
// Read 'man 2 mkdir'.
int myfs_mkdir(const char *path, mode_t mode){
	write_log("myfs_mkdir: %s\n",path);

    return 0;
}

// Delete a file.
// Read 'man 2 unlink'.
int myfs_unlink(const char *path){
	write_log("myfs_unlink: %s\n",path);

    return 0;
}

// Delete a directory.
// Read 'man 2 rmdir'.
int myfs_rmdir(const char *path){
    write_log("myfs_rmdir: %s\n",path);

    return 0;
}

// OPTIONAL - included as an example
// Flush any cached data.
int myfs_flush(const char *path, struct fuse_file_info *fi){
    int retstat = 0;

    write_log("myfs_flush(path=\"%s\", fi=0x%08x)\n", path, fi);

    return retstat;
}

// OPTIONAL - included as an example
// Release the file. There will be one call to release for each call to open.
int myfs_release(const char *path, struct fuse_file_info *fi){
    int retstat = 0;

    write_log("myfs_release(path=\"%s\", fi=0x%08x)\n", path, fi);

    return retstat;
}

// OPTIONAL - included as an example
// Open a file. Open should check if the operation is permitted for the given flags (fi->flags).
// Read 'man 2 open'.
static int myfs_open(const char *path, struct fuse_file_info *fi){
	if (strcmp(path, the_root_fcb.path) != 0)
		return -ENOENT;

	write_log("myfs_open(path\"%s\", fi=0x%08x)\n", path, fi);

	//return -EACCES if the access is not permitted.

	return 0;
}

static struct fuse_operations myfs_oper = {
	.getattr	= myfs_getattr,
	.readdir	= myfs_readdir,
	.open		= myfs_open,
	.read		= myfs_read,
	.create		= myfs_create,
	.utime 		= myfs_utime,
	.write		= myfs_write,
	.truncate	= myfs_truncate,
	.flush		= myfs_flush,
	.release	= myfs_release,
};


// Initialise the in-memory data structures from the store. If the root object (from the store) is empty then create a root fcb (directory)
// and write it to the store. Note that this code is executed outide of fuse. If there is a failure then we have failed toi initlaise the
// file system so exit with an error code.
void init_fs(){

	printf("init_fs\n");

	//Initialise the store.
	init_store();

	//if there is no root then craete a new one and store it to the unqlite store
	//otherwise, fetch the one that has already been created from the unqlite store
	if(!root_is_empty){

		printf("init_fs: root is not empty\n");

		//Fetch the fcb that the root object points at. We will probably need it.
		fetchFCBFromUnqliteStore( &(root_object.id), &the_root_fcb);

	} else {

		//Initialise and store an empty root fcb.
		printf("init_fs: root is empty\n");
		memset(&the_root_fcb, 0, sizeof(file_node));

		//See 'man 2 stat' and 'man 2 chmod'.
		the_root_fcb.mode |= S_IFDIR|S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH;
		the_root_fcb.mtime = time(0);
		the_root_fcb.uid = getuid();
		the_root_fcb.gid = getgid();
		the_root_fcb.size = 0;
		the_root_fcb.path = getcwd(the_root_fcb.path, PATH_MAX);

		//Generate a key for the_root_fcb and update the root object.
		uuid_generate(root_object.id);

		//the directory contents for the root directory
		dir_data root_dir_contents;

		//store the current directory as "." in the directory and ".." as the parent directory
		dir_entry *cwd = malloc(sizeof(dir_entry));
		cwd->path = CWD_STR;
		uuid_copy(root_object.id, cwd->fileNodeId);
		root_dir_contents.entries = &cwd;
		the_root_fcb.size++;
		root_dir_contents.entries++;

		//store the root fcb in the database store
		printf("init_fs: writing root fcb\n");
		storeFCBInUnqliteStore(&root_object.id, &the_root_fcb);

		//Store root object.
		printf("init_fs: writing updated root object\n");
		updateRootObject();
	}
}

void shutdown_fs(){
	unqlite_close(pDb);
}

int main(int argc, char *argv[]){
	int fuserc;
	struct myfs_state *myfs_internal_state;

	//Setup the log file and store the FILE* in the private data object for the file system.
	myfs_internal_state = malloc(sizeof(struct myfs_state));
    myfs_internal_state->logfile = init_log_file();

	//Initialise the file system. This is being done outside of fuse for ease of debugging.
	init_fs();

	fuserc = fuse_main(argc, argv, &myfs_oper, myfs_internal_state);

	//Shutdown the file system.
	shutdown_fs();

	return fuserc;
}

//fetches a file control block at the id given by the user
//into the address given by the user.
int fetchFCBFromUnqliteStore(uuid_t *data_id, file_node *buffer) {

	//tracks the error message returned from when fetching from the unqlite
	//datastore
	int rc;

	//number of bytes read back from the database
	unqlite_int64 nBytes;  //Data length.

	//this checks for errors before carrying out the fetch process
	//by checking the unqlite return code and also that the number of
	//bytes returned is the right number expected.
	rc = unqlite_kv_fetch(pDb,data_id,KEY_SIZE,NULL,&nBytes);

	if( rc != UNQLITE_OK ){
	  error_handler(rc);
	}
	if(nBytes!=sizeof(file_node)){
		printf("Data object has unexpected size. Doing nothing.\n");
		exit(-1);
	}

	//Fetch the fcb that the root object points at. We will probably need it.
	unqlite_kv_fetch(pDb,data_id,KEY_SIZE,buffer,&nBytes);

	return 1;
}

//stores a control block at the given id with the given data
int storeFCBInUnqliteStore(uuid_t *key_id, file_node *value_addr) {

		//store the file control block at the given address and record the result code
		int rc = unqlite_kv_store(pDb,key_id,KEY_SIZE,value_addr,sizeof(file_node));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
   			return -1;
		}

		return 1;
}

//updates the root object
int updateRootObject() {
	//write the root object to the database and check for errors
	int rc = write_root();

	//if the return code shows an error, then handle the error
	if( rc != UNQLITE_OK ){
  		error_handler(rc);
  	}
}

int fetchDirectoryDataFromUnqliteStore(uuid_t *data_id, dir_data *buffer){

//tracks the error message returned from when fetching from the unqlite
	//datastore
	int rc;

	//number of bytes read back from the database
	unqlite_int64 nBytes;  //Data length.

	//this checks for errors before carrying out the fetch process
	//by checking the unqlite return code and also that the number of
	//bytes returned is the right number expected.
	rc = unqlite_kv_fetch(pDb,data_id,KEY_SIZE,NULL,&nBytes);
	
	if( rc != UNQLITE_OK ){
	  error_handler(rc);
	}
	if(nBytes!=sizeof(dir_data)){
		printf("Data object has unexpected size. Doing nothing.\n");
		exit(-1);
	}

	//Fetch the fcb that the root object points at. We will probably need it.
	unqlite_kv_fetch(pDb,data_id,KEY_SIZE,buffer,&nBytes);
}

int fillStatWithFileNode(struct stat* destination, file_node* source){

	//device id
	//inode number
	destination->st_mode = source->mode; //protection
	//number of links
	destination->st_uid = source->uid; //user
	destination->st_gid = source->gid; //group
	//rdev (device id)
	destination->st_size = source->size; //size of file
	//blksize
	//number of blocks

	return 0;
}

int storeDirectoryDataFromUnqliteStore(uuid_t *key_id, dir_data *value_addr){

		//store the directory data at the given address and record the result code
		int rc = unqlite_kv_store(pDb,key_id,KEY_SIZE,value_addr,sizeof(dir_data));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
		}
}

int initNewFCB(const char* path, file_node* buff){

	//reset the space for the new file control block
	memset(buff, 0, sizeof(file_node));

	//copy the path into the file node path
	sprintf(buff->path, path);

    //generate a new unique id for the new file block
    uuid_generate(buff->data_id);

    //set the modification time to the current time
	buff->mtime = time(0);

    buff->uid = getuid(); //user
	buff->gid = getgid(); //group
	buff->mode |= S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IFREG; //set mode and protection
	buff->size = 0; //size of the file


	return 0;
}

int getFileNode(const char* path, file_node* fnode) {

	//the current file node being checked while navigating the tree
	file_node current_node = the_root_fcb;

	//the current directory data of the current node that we are checking
	dir_data current_dir_data;

	//the directory entry we are currently checking
	dir_entry curr_entry, *curr_entries;

	//the string for the path to the node we need to find
	char *pathBuff;

	//copy the contents of the path into the path buffer
	sprintf(pathBuff, path);

	//get the segments of the path so that we can navigate the directory tree
	char **pathSegs = *getPathArray(pathBuff);

	//while there are still path segments
	while(pathSegs) {

		//fetch the current directory data
		fetchDirectoryDataFromUnqliteStore(&current_node.data_id, &current_dir_data);

		//get the list of entries from the current directory data
		curr_entries = current_dir_data.entries;

		//while there are still directory entries left
		while(current_dir_data.entries) {

			//get the next entry in the collection of entries
			curr_entry = *curr_entries;

			//if the next entry's path matches the current path segment then it is the target
			//file that we are looking for then change the current node to the uuid found in the
			//directory entry and get the next segment.
			if(strcmp(curr_entry.path, *pathSegs) == 0) {

				fetchFCBFromUnqliteStore(&curr_entry.fileNodeId, &current_node);
				
				pathSegs++;

			//otherwise, increment the current entries pointer
			} else {
				curr_entries++;
			}

		}

	}

	return 0;
}

char** getPathArray(char* path){

	//set up a separate string buffer for the path
	//and the segment that was taken from strtok
	//and the collection of path segments
	char *pathBuff, *segment, **segmentColl, **start = segmentColl;

	//the path separator that will be used
	//for the delimiter of strtok
	const char* path_separator = "/";

	//copy the path into the string buffer
	sprintf(pathBuff, path);

	//get the initial segment
	segment = strtok(pathBuff, path_separator);

	//while there is a segment
	while(segment != NULL) {

		//add the segment we just found to the collection of segments
		*segmentColl++ = segment;
	}

	//restart the segment collection to the start of the array
	segmentColl = start;

	//return the address of the start of the segment array
	return segmentColl;
}