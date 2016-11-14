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

	//read the root
	read_root();

	//the fnode structure used for passing values to the stat structure
	file_node fnode;

	//if id of the node - this space is needed for getting the file node
	uuid_t nodeId;

	if(getFileNode(path, &fnode, &nodeId) == -1) {
		write_log("myfs_getattr - file not found\n");
		return -ENOENT;
	}

	//clear enough space for a stat by setting a stat's worth of space
	//all to 0 so that it can be used for a new stat struct
	memset(stbuf, 0, sizeof(struct stat));

	//fill the stat buff with the values in the fnode structutre
	fillStatWithFileNode(stbuf, &fnode);

	return 0;
}

// Read a directory.
// Read 'man 2 readdir'.
static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi){
	(void) offset;
	(void) fi;

	//write an entry to the log
	write_log("write_readdir(path=\"%s\", buf=0x%08x, filler=0x%08x, offset=%lld, fi=0x%08x)\n", path, buf, filler, offset, fi);

	//the file node of the directory to be read.
	file_node fnode;

	//the directory data of the directory that we are trying to read
	dir_data dirdata;

	//the directory entry that we are trying to fetch
	dir_entry dirent, *entries;	

	//if id of the node - this space is needed for getting the file node
	uuid_t nodeId;

	//the number of file links to the directory
	int size;

	//if there was no error for getting the file node with the given path
	//then 
	if(getFileNode(path, &fnode, &nodeId) == -1) {
		write_log("myfs_getattr - file not found");
		return -ENOENT;
	}

	//if the file given is not a directory then throw the error
	if(!IS_DIR(fnode.mode)) {
		write_log("readdir failed - %s is not a directory.\n", path);
		return -ENOTDIR;
	}

	//after we have done all of the checks, fill the directory data structure
	//with the data from the database
	fetchDirectoryDataFromUnqliteStore(&fnode.data_id, &dirdata);

	//if the directory that we are reading is the root then
	//do the filler for the "." and ".." entries as they are
	//kept separately from the store and then set the size
	//to the size of the root directory minus 2 so that it
	//remembers to ignore those two directories
	if(IS_ROOT(fnode.path)){
		//get the number of files in the directory
		size = fnode.size - 2;
		filler(buf, CWD_STR, NULL, 0);
		filler(buf, PD_STR, NULL, 0);
	} else {
		size = fnode.size;
	}

	//get the list of directory entries
	entries = dirdata.entries;

	//carry out the filler function on every file in the directory
	for(int i = 0; i < size; i++) {
		dirent = *entries;
		filler(buf, dirent.path, NULL, 0);
		entries++;
	}

	//reset the pointer
	entries -= size;

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

	write_log("myfs_create : trying to create %s\n", path);

	//the file name of the new file
	char pathBuff[strlen(path)], *filename;

    //file_node space for the root node to start and the new file block
    file_node parent, newFileBlock;

    //the ids of the parent and the child nodes
    uuid_t parentId;

    //the directory entry of the file into its parent directory
	dir_entry dirent;

	//the directory data of this parent
	dir_data dirdata;

	//clears the space before using it
	memset(&parent, 0, sizeof(file_node));
	memset(&newFileBlock, 0, sizeof(file_node));
	memset(&dirdata, 0, sizeof(dir_data));
	memset(&dirent, 0, sizeof(dir_entry));

    //copy the path into the path buff
    sprintf(pathBuff, path);

    //copies the file name without the leading '/'
    filename = basename(pathBuff);

    //fetch the parent object from the database
    getParentFileNode(path, &parent, &parentId);

    //initialise a new file block for the new file
    initNewFCB(filename, &newFileBlock);

    //length of the path
	int pathlen = strlen(path);

	//if the pathlength is too lo
	if(pathlen>=MY_MAX_PATH){
		write_log("myfs_create - ENAMETOOLONG");
		return -ENAMETOOLONG;
	}

	//copy the file name into the path held in the directory entry
	sprintf(dirent.path, filename);

	//make a directory entry to link this file to its parent
	makeDirent(filename, &parent , &dirent, &dirdata);

	struct fuse_context *context = fuse_get_context();

	//store the updated directory data for the parent node in the store
	storeDirectoryDataFromUnqliteStore(&parent.data_id, &dirdata);

	//store the file control block of the new file into the store
	storeFCBInUnqliteStore(&dirent.fileNodeId, &newFileBlock);

	//store the updated file control block of the parent directory into
	//the store
	storeFCBInUnqliteStore(&parentId, &parent);

	//update the root object
	updateRootObject();

    return 0;
}

// Set update the times (actime, modtime) for a file. This FS only supports modtime.
// Read 'man 2 utime'.
static int myfs_utime(const char *path, struct utimbuf *ubuf){
    write_log("myfs_utime(path=\"%s\", ubuf=0x%08x)\n", path, ubuf);

    file_node nodeToUpdate;

    uuid_t node_uuid;

    if(getFileNode(path, &nodeToUpdate, &node_uuid) != 0){
    	write_log("myfs_utime file not found\n");
    	return -ENOENT;
    }

    nodeToUpdate.mtime=ubuf->modtime;

    if(strcmp(nodeToUpdate.path, "/") == 0)
		the_root_fcb.mtime=ubuf->modtime;

	// Write the fcb to the store.

	storeFCBInUnqliteStore(&node_uuid, &nodeToUpdate);

    return 0;
}

// Write to a file.
// Read 'man 2 write'
static int myfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
    write_log("myfs_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n", path, buf, size, offset, fi);

    //the remaining size that we have to write
    size_t remaining_size = size, size_to_write;

    //the offsets for where the write starts in different blocks
    int indir_block_offset, small_block_offset, interior_block_offset, size_read = 0, single_indirect_call_needed = 1;

    //the file control block of the file that we are writing to.
    file_node file_to_write;

    //the uuid of where the file control block we found is in the store
    uuid_t file_uuid, data_block_uuid, single_indirect_uuid;

    //the data for the file we are writing to
    reg_data file_data;

    //the current block of data that we need
    data_block curr_block;

    //the current single indreict block that we need
    single_indirect indir_block;

    //if there was a problem locating the file then return ENOENT
    if(getFileNode(path, &file_to_write, &file_uuid) != 0){
    	write_log("myfs_write : error locating file %s\n", path);
    	return -ENOENT;
    }

    //if the size given to us was greater than the maximum file size then exit
	if(size >= MY_MAX_FILE_SIZE){
		write_log("myfs_write - EFBIG");
		return -EFBIG;
	}

	//the data block to write to the file data
	uint8_t data_block[MY_BLOCK_SIZE];

	//clear this data block by writing 0s to it all
	memset(&data_block, 0, MY_BLOCK_SIZE);

	//data id for the current block of memory
	uuid_t data_id;

	//set the data id to the data if of the file node
	uuid_copy(data_id, file_to_write.data_id);

	//clear the file data space
	memset(&file_data, 0, sizeof(reg_data));

	//if there is no uuid for the file's data block then generate a uuid for it
	if(UUID_IS_BLANK(data_id)) {

		uuid_generate(file_to_write.data_id);
		uuid_copy(data_id, file_to_write.data_id);

	//if the key does exist then copy everything into the file_data from the database
	} else
		fetchRegularFileDataFromUnqliteStore(&data_id, &file_data);

	//get the various offset values needed
	getOffsets(offset, &indir_block_offset, &small_block_offset, &interior_block_offset);

	//while there is data still left to write
	while(remaining_size != 0) {

		//if the remaining size left will not fill the block
		if(remaining_size < (MY_BLOCK_SIZE - interior_block_offset)) {
			
			//we write the remaining size to the block
			size_to_write = remaining_size;
			remaining_size = 0;

		} else {

			//set the size to write as the size of the rest of the block
			//from the interior block offset
			size_to_write = MY_BLOCK_SIZE - interior_block_offset;

			//decrease the remaining size by the amount we need to write
			remaining_size -= size_to_write;
		}

		//clear the data block
		memset(&curr_block, 0, sizeof(data_block));

		//if we are in direct blocks only at this stage then try to fetch the direct block and write to it
		if(indir_block_offset != - 1) {

			//get the uuid of the next direct block
			uuid_copy(data_block_uuid, file_data.direct_blocks[small_block_offset]);

			//if the block has not been created and so has a blank uuid then create a new uuid for it
			if(UUID_IS_BLANK(data_block_uuid)) {

				uuid_generate(file_data.direct_blocks[small_block_offset]);
				uuid_copy(data_block_uuid, file_data.direct_blocks[small_block_offset]);

			} else {

				//get the data block from the file
				fetchDataBlockFromUnqliteStore(&data_block_uuid, &curr_block);

			}

			//write the new data to the structure and then subsequently to the database
			memcpy(&curr_block.data[interior_block_offset], &buf[size_read], size_to_write);
			storeDataBlockFromUnqliteStore(&data_block_uuid, &curr_block);

			//if we have reached the end of the current indirect block then write it to the database and then
			//increment
			if(++interior_block_offset == MY_MAX_BLOCK_LIMIT) {
				indir_block_offset++;
				interior_block_offset = 0;
				single_indirect_call_needed = 1;
			}

		//if we are using indirect blocks
		} else {

			//if we are either writing for the first time or we are currently not writing to a single
			//indirect then create a new data space for it and make a key for it
			if(single_indirect_call_needed == 1) {
				memset(&indir_block, 0, sizeof(single_indirect));

				uuid_copy(single_indirect_uuid, file_data.indirect_blocks[indir_block_offset]);

					if(UUID_IS_BLANK(single_indirect_uuid)) {
						uuid_generate(file_data.indirect_blocks[indir_block_offset]);
						uuid_copy(single_indirect_uuid, file_data.indirect_blocks[indir_block_offset]);

					} else {

						fetchSingleIndirectBlockFromUnqliteStore(&single_indirect_uuid, &indir_block);

					}

				single_indirect_call_needed = 0;
			}

			//get the uuid of the next direct block
			uuid_copy(data_block_uuid, indir_block.data_blocks[small_block_offset]);

			//if the block has not been created and so has a blank uuid then create a new uuid for it
			if(UUID_IS_BLANK(data_block_uuid)) {

				uuid_generate(file_data.direct_blocks[small_block_offset]);
				uuid_copy(data_block_uuid, file_data.direct_blocks[small_block_offset]);

			} else {

				//get the data block from the file
				fetchDataBlockFromUnqliteStore(&data_block_uuid, &curr_block);

			}

			//if we have reached the end of the current indirect block then write it to the database and then
			//increment
			if(++interior_block_offset == MY_MAX_BLOCK_LIMIT) {
				storeSingleIndirectBlockFromUnqliteStore(&single_indirect_uuid, &indir_block);
				indir_block_offset++;
				interior_block_offset = 0;
				single_indirect_call_needed = 1;
			}

		}

	}
	

	// // Is there a data block?
	// if(uuid_compare(zero_uuid,*data_id)==0){
	// 	// GEnerate a UUID fo rhte data blocl. We'll write the block itself later.
	// 	uuid_generate(the_root_fcb.data_id);
	// }else{
	// 	// First we will check the size of the obejct in the store to ensure that we won't overflow the buffer.
	// 	unqlite_int64 nBytes;  // Data length.
	// 	int rc = unqlite_kv_fetch(pDb,data_id,KEY_SIZE,NULL,&nBytes);
	// 	if( rc!=UNQLITE_OK || nBytes!= MY_MAX_FILE_SIZE){
	// 		write_log("myfs_write - EIO");
	// 		return -EIO;
	// 	}

	// 	// Fetch the data block from the store.
	// 	unqlite_kv_fetch(pDb,data_id,KEY_SIZE,&data_block,&nBytes);
	// 	// Error handling?
	// }

	// // Write the data in-memory.
 //    int written = snprintf(data_block, MY_MAX_FILE_SIZE, buf);

	// // Write the data block to the store.
	// int rc = unqlite_kv_store(pDb,data_id,KEY_SIZE,&data_block,MY_MAX_FILE_SIZE);
	// if( rc != UNQLITE_OK ){
	// 	write_log("myfs_write - EIO");
	// 	return -EIO;
	// }

	// // Update the fcb in-memory.
	// the_root_fcb.size=written;
	// time_t now = time(NULL);
	// the_root_fcb.mtime=now;
	// the_root_fcb.ctime=now;

	// // Write the fcb to the store.
 //    rc = unqlite_kv_store(pDb,&(root_object.id),KEY_SIZE,&the_root_fcb,sizeof(file_node));
	// if( rc != UNQLITE_OK ){
	// 	write_log("myfs_write - EIO");
	// 	return -EIO;
	// }

    // return written;
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
	
	write_log("mkdir %s\n", path);

	//the file blocks for the new directory and the parent
	file_node newFileBlock, parent;

	//the id of where the parent file node is stored in the database
	uuid_t parent_id;

    //the directory entry of the file into its parent directory
	dir_entry dirent;

	//the directory data of this parent
	dir_data dirdata, newDirData;

	char pathBuff[strlen(path)];

    sprintf(pathBuff, path);

    //copies the file name without the leading '/'
    char* filename = basename(pathBuff);

    //length of the path
	int pathlen = strlen(path);

	//if the pathlength is too long then return ENAMETOOLONG
	if(pathlen>=MY_MAX_PATH){
		write_log("myfs_create - ENAMETOOLONG");
		return -ENAMETOOLONG;
	}

	//clear the memory space before allocating it
	memset(&parent, 0, sizeof(file_node));
	memset(&newFileBlock, 0, sizeof(file_node));
	memset(&dirdata, 0, sizeof(dir_data));
	memset(&dirent, 0, sizeof(dir_entry));
	
	//if there was an error when getting the file node then return file not found
	if(getParentFileNode(path, &parent, &parent_id) != 0) {
		write_log("ENOENT %s parent not found\n");
		return -ENOENT;
	}

	//make a new file control block for the new directory
	initNewFCB(filename, &newFileBlock);

	//add that this file is a directory to the mode
	newFileBlock.mode = mode | S_IFDIR;

	//make a directory entry to link this directory to its parent
	makeDirent(filename, &parent ,&dirent, &dirdata);

	//make a CWD (".") directory entry for this directory
	makeCWDdirent(&dirent.fileNodeId, &newDirData);

	//make a directory entry that links the parent directory
	//to this directory ("..")
	makePDdirent(&parent_id, &newDirData);

	//since we only have two entries in this directory (itself and its parent),
	//set its size to 2
	newFileBlock.size = 2;

	struct fuse_context *context = fuse_get_context();

	//store the directory data of the parent into the store
	storeDirectoryDataFromUnqliteStore(&parent.data_id, &dirdata);

	//store the directory data of the new directory into the store
	storeDirectoryDataFromUnqliteStore(&newFileBlock.data_id, &newDirData);

	//store the file node of the new directory into the store
	storeFCBInUnqliteStore(&dirent.fileNodeId, &newFileBlock);

	//store the updated file node of the parent into the store
	storeFCBInUnqliteStore(&parent_id, &parent);

	//update the root object
	updateRootObject();

	//if there were no errors, return 0
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
	.mkdir 		= myfs_mkdir,
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
		the_root_fcb.size = 2;
		sprintf(the_root_fcb.path, "/");

		//Generate a key for the_root_fcb and update the root object.
		uuid_generate(root_object.id);

		//the directory contents for the root directory
		dir_data root_dir_contents;

		//the unique id of the directory data 
		uuid_generate(the_root_fcb.data_id);

		//store the directory data in the database
		printf("init_fs: storing directory data\n");
		storeDirectoryDataFromUnqliteStore(&the_root_fcb.data_id, &root_dir_contents);

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
	rc = unqlite_kv_fetch(pDb,*data_id,KEY_SIZE,NULL,&nBytes);

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
		int rc = unqlite_kv_store(pDb,*key_id,KEY_SIZE,value_addr,sizeof(file_node));
		
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
	rc = unqlite_kv_fetch(pDb,*data_id,KEY_SIZE,NULL,&nBytes);
	
	if( rc != UNQLITE_OK ){
	  error_handler(rc);
	}
	if(nBytes!=sizeof(dir_data)){
		printf("Data object has unexpected size. Doing nothing.\n");
		exit(-1);
	}

	//Fetch the fcb that the root object points at. We will probably need it.
	unqlite_kv_fetch(pDb,data_id,KEY_SIZE,buffer,&nBytes);

	return 1;
}


int storeDirectoryDataFromUnqliteStore(uuid_t *key_id, dir_data *value_addr){

		//store the directory data at the given address and record the result code
		int rc = unqlite_kv_store(pDb,*key_id,KEY_SIZE,value_addr,sizeof(dir_data));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
		}
}

int fetchRegularFileDataFromUnqliteStore(uuid_t *data_id, reg_data *buffer){

	//tracks the error message returned from when fetching from the unqlite
	//datastore
	int rc;

	//number of bytes read back from the database
	unqlite_int64 nBytes;  //Data length.

	//this checks for errors before carrying out the fetch process
	//by checking the unqlite return code and also that the number of
	//bytes returned is the right number expected.
	rc = unqlite_kv_fetch(pDb,*data_id,KEY_SIZE,NULL,&nBytes);
	
	if( rc != UNQLITE_OK ){
	  error_handler(rc);
	}
	if(nBytes!=sizeof(reg_data)){
		printf("Data object has unexpected size. Doing nothing.\n");
		exit(-1);
	}

	unqlite_kv_fetch(pDb,data_id,KEY_SIZE,buffer,&nBytes);

	return 1;
}

int storeRegularFileDataFromUnqliteStore(uuid_t *key_id, reg_data *value_addr){

		//store the regulat filed data structure at the given address and record the result code
		int rc = unqlite_kv_store(pDb,*key_id,KEY_SIZE,value_addr,sizeof(reg_data));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
		}
}

int fetchSingleIndirectBlockFromUnqliteStore(uuid_t *data_id, single_indirect *buffer){

	//tracks the error message returned from when fetching from the unqlite
	//datastore
	int rc;

	//number of bytes read back from the database
	unqlite_int64 nBytes;  //Data length.

	//this checks for errors before carrying out the fetch process
	//by checking the unqlite return code and also that the number of
	//bytes returned is the right number expected.
	rc = unqlite_kv_fetch(pDb,*data_id,KEY_SIZE,NULL,&nBytes);
	
	if( rc != UNQLITE_OK ){
	  error_handler(rc);
	}
	if(nBytes!=sizeof(single_indirect)){
		printf("Data object has unexpected size. Doing nothing.\n");
		exit(-1);
	}

	unqlite_kv_fetch(pDb,data_id,KEY_SIZE,buffer,&nBytes);

	return 1;
}

int storeSingleIndirectBlockFromUnqliteStore(uuid_t *key_id, single_indirect *value_addr){

		//store the single indirect at the given address and record the result code
		int rc = unqlite_kv_store(pDb,*key_id,KEY_SIZE,value_addr,sizeof(single_indirect));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
		}
}

int fetchDataBlockFromUnqliteStore(uuid_t *data_id, data_block *buffer){

	//tracks the error message returned from when fetching from the unqlite
	//datastore
	int rc;

	//number of bytes read back from the database
	unqlite_int64 nBytes;  //Data length.

	//this checks for errors before carrying out the fetch process
	//by checking the unqlite return code and also that the number of
	//bytes returned is the right number expected.
	rc = unqlite_kv_fetch(pDb,*data_id,KEY_SIZE,NULL,&nBytes);
	
	if( rc != UNQLITE_OK ){
	  error_handler(rc);
	}
	if(nBytes!=sizeof(data_block)){
		printf("Data object has unexpected size. Doing nothing.\n");
		exit(-1);
	}

	unqlite_kv_fetch(pDb,data_id,KEY_SIZE,buffer,&nBytes);

	return 1;
}

int storeDataBlockFromUnqliteStore(uuid_t *key_id, data_block *value_addr){

		//store the data block at the given address and record the result code
		int rc = unqlite_kv_store(pDb,*key_id,KEY_SIZE,value_addr,sizeof(data_block));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
		}
}

int fillStatWithFileNode(struct stat* destination, file_node* source){

	write_log("Source path : %s\n", source->path);
	//device id
	//inode number
	destination->st_mode = source->mode; //protection
	//number of links
	destination->st_uid = source->uid; //user
	destination->st_gid = source->gid; //group
	//rdev (device id)
	destination->st_size = source->size; //size of file
	destination->st_atim.tv_sec = source->atime;
	destination->st_ctim.tv_sec = source->ctime;
	destination->st_mtim.tv_sec = source->mtime;
	//blksize
	//number of blocks

	return 0;
}


int initNewFCB(const char* path, file_node* buff){

	//reset the space for the new file control block
	memset(buff, 0, sizeof(file_node));

	//copy the path into the file node path
	strcpy(buff->path, path);

    //generate a new unique id for the new file block
    uuid_generate(buff->data_id);

    //the current time
    time_t currentTime;
	time(&currentTime);

    buff->uid = getuid(); //user
	buff->gid = getgid(); //group
	buff->mode |= S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IFREG; //set mode and protection
	buff->size = 0; //size of the file
	buff->atime = currentTime;
	buff->ctime = currentTime;
	buff->mtime = currentTime;

	return 0;
}

int getFileNode(const char* path, file_node* fnode, uuid_t* fnode_uuid) {

	//the current file node being checked while navigating the tree
	file_node current_node = the_root_fcb;

	//the current directory data of the current node that we are checking
	dir_data current_dir_data;

	//the directory entry we are currently checking
	dir_entry curr_entry;

	//the string for the path to the node we need to find
	char pathBuff[strlen(path)];

	//the delimiter to use for the strtok process
	const char* PATH_SEPARATOR = "/";

	//flag for determining if a corresponding directory entry has been found for a given
	//path
	int path_error = FALSE, dirent_found = FALSE, numberOfEntries = 0;

	//copy the contents of the path into the path buffer
	sprintf(pathBuff, path);

	//if the path given is the path for the root fcb then
	//return the fcb
	if(IS_ROOT(path)) {
		write_log("using root\n");
		memcpy(fnode, &the_root_fcb, sizeof(file_node));
		return 0;
	} else {
		write_log("not using root - using %s\n", pathBuff);
	}

	//get the segments of the path so that we can navigate the directory tree
	char *pathSeg = strtok(pathBuff, PATH_SEPARATOR);
	
	//while there are still path segments
	while(pathSeg != NULL && path_error == FALSE) {

		dirent_found = FALSE;

		//if the current node is not a directory then do not try and fetch its directory data
		if(!IS_DIR(current_node.mode)) {

			write_log("Locating file node failed : %s is not a directory.\n", current_node.path);
			printf("%s is not a directory.\n", current_node.path);
			return -1;

		} else {

			//fetch the current directory data
			fetchDirectoryDataFromUnqliteStore(&current_node.data_id, &current_dir_data);

			if(IS_ROOT(current_node.path)) {
				numberOfEntries = current_node.size - 2;
			}
			else{
				numberOfEntries = current_node.size;
			}

			//get the list of entries from the current directory data
			dir_entry curr_entries[numberOfEntries];
			memset(curr_entries, 0, sizeof(dir_entry) * numberOfEntries);
			memcpy(&curr_entries, &current_dir_data.entries, sizeof(dir_entry) * numberOfEntries);

			//while there are still directory entries left and the directory
			//that we need has not been found
			for(int i = 0; i < numberOfEntries; i++) {

				//get the next entry in the collection of entries
				curr_entry = curr_entries[i];

				//if the next entry's path matches the current path segment then it is the target
				//file that we are looking for then change the current node to the uuid found in the
				//directory entry and get the next segment.
				if(strcmp(curr_entry.path, pathSeg) == 0) {

					//fetch the corresponding file block for the given path from
					//the store
					memset(&current_node, 0 , sizeof(file_node));
					fetchFCBFromUnqliteStore(&curr_entry.fileNodeId, &current_node);
					
					//set the dirent_found flag to true
					dirent_found = TRUE;

					//set the directory entry flag found to true
					break;
				}

			}

		}

		//if a next directory entry was found, set the path segment to the next token
		//get the next segment of the path
		if(dirent_found == TRUE) {
			pathSeg = strtok(NULL, PATH_SEPARATOR);
		} else {
			path_error = TRUE;
		}

	}

	//if there was no error then copy the contents of the node we found into the
	//space given by the user
	if(path_error == FALSE) {
		memset(fnode, 0, sizeof(file_node));
		memcpy(fnode, &current_node, sizeof(file_node));
		memset(fnode_uuid, 0, sizeof(uuid_t));
		memcpy(fnode_uuid, &curr_entry.fileNodeId, sizeof(uuid_t));
		//fnode_uuid = &curr_entry.fileNodeId;
	}

	//otherwise return an error
	else
		return -1;

	return 0;
}

int getParentFileNode(const char* path, file_node *buffer, uuid_t* buff_uuid){

	int lengthOfPath = strlen(path);

	char pathBuff[lengthOfPath];

	sprintf(pathBuff, path);

	char* parentPath = dirname(pathBuff);

	getFileNode(parentPath, buffer, buff_uuid);

}

int makeDirent(char* filename, file_node *parentNode, dir_entry *dirent, dir_data *dirdata) {

	sprintf(dirent->path, filename);

	uuid_generate(dirent->fileNodeId);

	fetchDirectoryDataFromUnqliteStore(&parentNode->data_id, dirdata);

	//the index of where the new entry will go in the array of directory entries
	//in the directory data
	int new_entry_position;

	if(IS_ROOT(parentNode->path)) {
		new_entry_position = parentNode->size - 2;
		the_root_fcb.size++;
	} else {
		new_entry_position = parentNode->size;
	}

	memcpy(&dirdata->entries[new_entry_position], dirent, sizeof(dir_entry));

	parentNode->size++;

}

int makeCWDdirent(uuid_t *cwdId, dir_data *newDirData){

	dir_entry cwdDirent;
	memset(&cwdDirent, 0, sizeof(dir_entry));

	uuid_copy(cwdDirent.fileNodeId, *cwdId);
	sprintf(cwdDirent.path, CWD_STR);

	memcpy(&newDirData->entries[0], &cwdDirent, sizeof(dir_entry));

}

int makePDdirent(uuid_t *pdId, dir_data *newDirData){

	dir_entry pdDirent;
	memset(&pdDirent, 0, sizeof(dir_entry));

	uuid_copy(pdDirent.fileNodeId, *pdId);
	sprintf(pdDirent.path, PD_STR);

	memcpy(&newDirData->entries[1], &pdDirent, sizeof(dir_entry));

}

int getOffsets(off_t offset, int *indir_block_offset, int *small_block_offset, int *interior_block_offset) {

	//if the offset starts after the end of the direct blocks
	if(offset > MY_DIRECT_BLOCKS_SIZE) {
		//determines which indirect block we need
		*indir_block_offset = (offset - MY_DIRECT_BLOCKS_SIZE) / MY_SINGLE_INDIRECT_BLOCK_SIZE;

		//determines which block in the direct block we start
		*small_block_offset = (offset - MY_DIRECT_BLOCKS_SIZE - (indir_block_offset * MY_SINGLE_INDIRECT_BLOCK_SIZE)) / MY_BLOCK_SIZE;

		//where in the block of memory we start writing
		*interior_block_offset = (offset - MY_DIRECT_BLOCKS_SIZE - (indir_block_offset * MY_SINGLE_INDIRECT_BLOCK_SIZE) - (small_block_offset * MY_BLOCK_SIZE));

	} else {

		//no indirect blocks yet so we set this to -1
		*indir_block_offset = -1;

		//determines which direct block we are starting from
		*small_block_offset = offset / MY_BLOCK_SIZE;

		//get where we need to start inside of the data block
		*interior_block_offset = offset - (MY_BLOCK_SIZE * small_block_offset);

	}

	return 0;
}