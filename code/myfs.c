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

	//clear the spaces
	memset(&fnode, 0, sizeof(file_node));
	memset(&nodeId, 0, sizeof(uuid_t));

	if(getFileNode(path, &fnode, &nodeId) == -ENOENT) {
		write_log("myfs_getattr - file not found\n");
		return -ENOENT;
	}

	//clear enough space for a stat by setting a stat's worth of space
	//all to 0 so that it can be used for a new stat struct
	memset(stbuf, 0, sizeof(struct stat));

	//fill the stat buff with the values in the fnode structutre
	fillStatWithFileNode(stbuf, &fnode);

	write_log("filled in stat for %s successfully\n", fnode.path);

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

	memset(&fnode, 0, sizeof(fnode));
	memset(&dirdata, 0, sizeof(dir_data));
	memset(&fnode, 0, sizeof(fnode));

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

	int stepper = 0, limit = 0;
	//carry out the filler function on every file in the directory
	while(limit < size) {

		if(entryIsInFreeList(&dirdata, &stepper) == -1) {
			dirent = entries[stepper];
			filler(buf, dirent.path, NULL, 0);
			limit++;
		}

		stepper++;
	}


	// //reset the pointer
	// entries -= size;

	return 0;
}

// Read a file.
// Read 'man 2 read'.
static int myfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	(void) fi;

	write_log("myfs_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n", path, buf, size, offset, fi);
	
    //the remaining size that we have to read
    size_t remaining_size = size, size_to_read;

    //the offsets for where the read starts in different blocks
    int indir_block_offset, small_block_offset, interior_block_offset, size_written = 0, single_indirect_call_needed = 1;

    //the file control block of the file that we are writing to.
    file_node file_to_read;

    //the uuid of where the file control block we found is in the store
    uuid_t file_uuid, data_block_uuid, single_indirect_uuid;

    //the data for the file we are writing to
    reg_data file_data;

    //the current block of data that we need
    data_block curr_block;

    //the current single indreict block that we need
    single_indirect indir_block;

    //if there was a problem locating the file then return ENOENT
    if(getFileNode(path, &file_to_read, &file_uuid) != 0){
    	write_log("myfs_read : error locating file %s\n", path);
    	return -ENOENT;
    }

    //if the size given to us was greater than the maximum file size then exit
	// if(size >= MY_MAX_FILE_SIZE){
	// 	write_log("Size too large\nSize : %d\nFile to read size : %d\n", size, file_to_read.size);
	// 	write_log("myfs_read - EFBIG\n");
	// 	return -EFBIG;
	// }

	//the data block to read to the file data
	uint8_t data_block[MY_BLOCK_SIZE];

	//clear this data block by writing 0s to it all
	memset(&data_block, 0, MY_BLOCK_SIZE);

	//data id for the current block of memory
	uuid_t data_id;

	//set the data id to the data if of the file node
	uuid_copy(data_id, file_to_read.data_id);

	//clear the file data space
	memset(&file_data, 0, sizeof(reg_data));

	write_log("uuid is blank : %d\n", UUID_IS_BLANK(data_id));
	//if there is no uuid for the file's data block then generate a uuid for it
	if(UUID_IS_BLANK(data_id)) {

		write_log("myfs_read - file empty - key does not exist.\n");
		return 0;

	//if the key does exist then copy everything into the file_data from the database
	} else {
		write_log("trying to fetch file data with key : %s\n", data_id);
		fetchRegularFileDataFromUnqliteStore(&data_id, &file_data);
		write_log("fetched successfully\n");
	}

	//get the various offset values needed
	getOffsets(offset, &indir_block_offset, &small_block_offset, &interior_block_offset);

	//while there is data still left to write
	while(remaining_size != 0) {

		write_log("Read remaining_size : %d\n", remaining_size);
		write_log("\n\nIndirect block offset : %d Small block offset : %d Interior Block Offset : %d\n\n", indir_block_offset, small_block_offset, interior_block_offset);

		getSizeToRead(&size_to_read, &remaining_size, &interior_block_offset);

		//clear the data block
		memset(&curr_block, 0, sizeof(data_block));

		//if we are in direct blocks only at this stage then try to fetch the direct block and write to it
		if(indir_block_offset == -1) {

			if (getReadDataBlockData(&curr_block, &data_block_uuid, &file_data.direct_blocks[small_block_offset]) == -1) {
				return size_written;
			}

			//write the new data to the structure and then subsequently to the database
			memcpy(&buf[size_written], &curr_block.data[interior_block_offset], size_to_read);
			storeDataBlockFromUnqliteStore(&data_block_uuid, &curr_block);

			//if we have reached the end of the current indirect block then write it to the database and then
			//increment
			if(++small_block_offset == MY_MAX_BLOCK_LIMIT) {
				indir_block_offset++;
				small_block_offset = 0;
				single_indirect_call_needed = 1;
			}

			//reset the data block index back to the start
			interior_block_offset = 0;

		//if we are using indirect blocks
		} else {

			//if we are either writing for the first time or we are currently not writing to a single
			//indirect then create a new data space for it and make a key for it
			if(single_indirect_call_needed == 1) {

				//try to get the next single indirect block. If there is not another one then write back the
				//number of bytes that we read.
				if(getReadSingleIndirectBlock(&indir_block, &single_indirect_uuid, &file_data.indirect_blocks[indir_block_offset]) == -1){
					return size_written;
				}
				
				//we don't need to make another database call again until we reach the end of this indirect block's
				//set of data blocks, so set this flag to 0
				single_indirect_call_needed = 0;
			}

			//get the uuid of the next direct block
			if((getReadDataBlockData(&curr_block, &data_block_uuid, &indir_block.data_blocks[small_block_offset]) == -1)) {
				return size_written;
			}

			//write the new data to the structure and then subsequently to the database
			memcpy(&buf[size_written], &curr_block.data[interior_block_offset], size_to_read);
			storeDataBlockFromUnqliteStore(&data_block_uuid, &curr_block);

			//if we have reached the end of the current indirect block then write it to the database and then
			//increment
			if(++small_block_offset == MY_MAX_BLOCK_LIMIT) {
				storeSingleIndirectBlockFromUnqliteStore(&single_indirect_uuid, &indir_block);
				indir_block_offset++;
				small_block_offset = 0;

				//set this flag to one so that we know that we have to make another data base call for
				//the next indirect block
				single_indirect_call_needed = 1;
			}

			//reset the data block index back to the start
			interior_block_offset = 0;

		}

		//increment the size that we have written to the file by the amount that was read in from
		//the buffer to read
		size_written += size_to_read;
	}

	return size_written;

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

    memset(&nodeToUpdate, 0, sizeof(file_node));
    memset(&node_uuid, 0, sizeof(uuid_t));

    if(getFileNode(path, &nodeToUpdate, &node_uuid) != 0){
    	write_log("myfs_utime file not found\n");
    	return -ENOENT;
    }

    nodeToUpdate.mtime=ubuf->modtime;
    nodeToUpdate.atime=ubuf->actime;

    if(strcmp(nodeToUpdate.path, "/") == 0) {
		the_root_fcb.mtime=ubuf->modtime;
		the_root_fcb.mtime=ubuf->actime;
    }

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
		write_log("myfs_write - EFBIG\n");
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

	write_log("uuid is blank : %d\n", UUID_IS_BLANK(data_id));
	//if there is no uuid for the file's data block then generate a uuid for it
	if(UUID_IS_BLANK(data_id)) {

		uuid_generate(file_to_write.data_id);
		uuid_copy(data_id, file_to_write.data_id);

	//if the key does exist then copy everything into the file_data from the database
	} else {
		write_log("trying to fetch file data\n");
		fetchRegularFileDataFromUnqliteStore(&data_id, &file_data);
		write_log("fetched successfully\n");
	}

	//get the various offset values needed
	getOffsets(offset, &indir_block_offset, &small_block_offset, &interior_block_offset);

	//while there is data still left to write
	while(remaining_size != 0) {

	write_log("\n\nIndirect block offset : %d Small block offset : %d Interior Block Offset : %d\n\n", indir_block_offset, small_block_offset, interior_block_offset);
	write_log("remaining size : %d\n", remaining_size);

		//get the size that we are writing to the file
		getSizeToRead(&size_to_write, &remaining_size, &interior_block_offset);

		//clear the data block
		memset(&curr_block, 0, sizeof(data_block));

		//if we are in direct blocks only at this stage then try to fetch the direct block and write to it
		if(indir_block_offset == - 1) {

			//get the uuid of the next direct block and get the data that corresponds to it
			//if it exists
			getWriteDataBlockData(&curr_block, &data_block_uuid, &file_data.direct_blocks[small_block_offset]);

			write_log("Size to write : %d\n", size_to_write);
			//write the new data to the structure and then subsequently to the database
			memcpy(&curr_block.data[interior_block_offset], &buf[size_read], size_to_write);
			write_log("data written : %s\n", curr_block.data);
			storeDataBlockFromUnqliteStore(&data_block_uuid, &curr_block);

			//if we have reached the end of the current indirect block then write it to the database and then
			//increment
			if(++small_block_offset == MY_MAX_BLOCK_LIMIT) {
				indir_block_offset++;
				small_block_offset = 0;
				single_indirect_call_needed = 1;
			}

			//reset the data block index back to the start
			interior_block_offset = 0;

		//if we are using indirect blocks
		} else {

			//if we are either writing for the first time or we are currently not writing to a single
			//indirect then create a new data space for it and make a key for it
			if(single_indirect_call_needed == 1) {

				getWriteSingleIndirectBlock(&indir_block, &single_indirect_uuid, &file_data.indirect_blocks[indir_block_offset]);

				single_indirect_call_needed = 0;
			}

			//get the uuid of the next direct block and data of the next block
			getWriteDataBlockData(&curr_block, &data_block_uuid, &indir_block.data_blocks[small_block_offset]);

			//write the new data to the structure and then subsequently to the database
			memcpy(&curr_block.data[interior_block_offset], &buf[size_read], size_to_write);
			storeDataBlockFromUnqliteStore(&data_block_uuid, &curr_block);

			//if we have reached the end of the current indirect block then write it to the database and then
			//increment
			if(++small_block_offset == MY_MAX_BLOCK_LIMIT) {
				storeSingleIndirectBlockFromUnqliteStore(&single_indirect_uuid, &indir_block);
				indir_block_offset++;
				small_block_offset = 0;
				single_indirect_call_needed = 1;
			}

			//reset the data block index back to the start
			interior_block_offset = 0;

		}

		//increment the size that we have read in from the buffer to write
		//by how much we wrote to the file
		size_read += size_to_write;
	}

	//store the last data block and single indirect block that we used
	storeDataBlockFromUnqliteStore(&data_block_uuid, &curr_block);
	storeSingleIndirectBlockFromUnqliteStore(&single_indirect_uuid, &indir_block);

	//if the file increased by size in this write then change the size
	//of the file size in the meta data
	if(file_to_write.size < offset + size)
		file_to_write.size = offset + size;

	//store the file's data and file control block in the file store
	storeRegularFileDataFromUnqliteStore(&file_to_write.data_id, &file_data);
	storeFCBInUnqliteStore(&file_uuid, &file_to_write);

	write_log("myfs writing to uuid : %s\n", file_uuid);

	return size_read;
}

// Set the size of a file.
// Read 'man 2 truncate'.
int myfs_truncate(const char *path, off_t newsize){
    write_log("myfs_truncate(path=\"%s\", newsize=%lld)\n", path, newsize);

    //check to see if the new size exceeds the max file size or not
	if(newsize >= MY_MAX_FILE_SIZE){
		write_log("myfs_truncate - EFBIG");
		return -EFBIG;
	}

	//the file node for the file that we are performing truncate on
    file_node fnode;

    //the uuid of where the file control block is stored in the database
    uuid_t file_uuid;

    //the result code returned from trying to fetch the file control
    //block from the database
    int rc = 0;

    //if the block was not fetched successfully then return the error code
    if((rc = getFileNode(path, &fnode, &file_uuid)) != 0) {
    	return rc;
    }

    //if the user requested to make a bigger size file, then make the file size larger by adding 0s
    //onto the end of the file until the last character, which should be -1 to denote the end of the file
    if(fnode.size < newsize) {

    	//the size that we are adding to the file
    	size_t addSize = newsize - fnode.size;

    	//the start position of where we are adding 0s to is the current
    	//end of the file, which is denoted by the size of the file
    	off_t startPosition = fnode.size;

    	//the buffer of 0s and the EOF -1 character to add
    	char buffdata[addSize];

    	//set the array to a collection of 0s
    	memset(&buffdata, 0, addSize);
    	
    	//set the last piece of data in the array as a -1 to denote the EOF
    	buffdata[addSize - 1] = -1;

    	//convert the data into a const char* so that it can be passed to the write function
    	const char* bufPointer = buffdata;

    	//empty placeholder for fuse info
		struct fuse_file_info fi;    	

    	//write the buffer to the file
    	myfs_write(path, bufPointer, addSize, startPosition, &fi);

    //if the new size of smaller than the current size then write a single -1 to the file where the file ends
    } else if (fnode.size > newsize) {

    	char EOFmarker = -1;

    	//EOFmarker in buffer form for the writer function
    	const char* data_pointer = &EOFmarker;

    	//the position of where we are placing the end of file character
    	off_t EOFPosition = newsize;

    	//the amount we are writing to the file
    	size_t write_size = 1;

    	//empty placeholder for fuse info
		struct fuse_file_info fi;  

		//write the eof character to the buffer
    	myfs_write(path, data_pointer, write_size, EOFPosition, &fi);

    }

    //set the new size of the file control block
    fnode.size = newsize;

    //store the file control block back into the database
    storeFCBInUnqliteStore(&file_uuid, &fnode);

	return 0;
}

// Set permissions.
// Read 'man 2 chmod'.
int myfs_chmod(const char *path, mode_t mode){
    write_log("myfs_chmod(fpath=\"%s\", mode=0%03o)\n", path, mode);

    file_node fcb;
    memset(&fcb, 0, sizeof(file_node));
    uuid_t fcbid;

    if(getFileNode(path, &fcb, &fcbid) == -ENOENT)
    	return -ENOENT;

    fcb.mode = mode;

	storeFCBInUnqliteStore(&fcbid, &fcb);

    return 0;
}

// Set ownership.
// Read 'man 2 chown'.
int myfs_chown(const char *path, uid_t uid, gid_t gid){
    write_log("myfs_chown(path=\"%s\", uid=%d, gid=%d)\n", path, uid, gid);

    file_node fcb;
    memset(&fcb, 0, sizeof(file_node));
    uuid_t fcbid;

    if(getFileNode(path, &fcb, &fcbid) == -ENOENT)
    	return -ENOENT;

    fcb.uid = uid;
    fcb.gid = gid;

    storeFCBInUnqliteStore(&fcbid, &fcb);

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

    //generate a new unique id for the new file block
    uuid_generate(newFileBlock.data_id);

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

	//the parent directory of the file that we are trying to delete
	file_node parent;

	//the uuid of where the fcb of the parent is stored in the database
	uuid_t parentId;

	//reset the parent node before filling it with the data from the
	//get parent function
	memset(&parent, 0, sizeof(file_node));
	getParentFileNode(path, &parent, &parentId);

	//remove the directory entry given in the path from the parent directory. Return
	//-ENOENT if the directory is not found
	if(removeDirent(path, &parent) == -ENOENT) {
		return -ENOENT;
	}

	//decrease the size of the parent by 1
	parent.size--;

	//store the parent at its id in the store
	storeFCBInUnqliteStore(&parentId, &parent);

    return 0;
}

// Delete a directory.
// Read 'man 2 rmdir'.
int myfs_rmdir(const char *path){
    write_log("myfs_rmdir: %s\n",path);

	//the parent directory of the file that we are trying to delete
	file_node parent;

	//the uuid of where the fcb of the parent is stored in the database
	uuid_t parentId;

	//reset the parent node before filling it with the data from the
	//get parent function
	memset(&parent, 0, sizeof(file_node));
	getParentFileNode(path, &parent, &parentId);

	//remove the directory entry given in the path from the parent directory. Return
	//-ENOENT if the directory is not found
	if(removeDirent(path, &parent) == -ENOENT) {
		return -ENOENT;
	}

	//decrease the size of the parent by 1
	parent.size--;

	//store the parent at its id in the store
	storeFCBInUnqliteStore(&parentId, &parent);

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
	.rmdir = myfs_rmdir,
	.unlink = myfs_unlink,
	.chmod = myfs_chmod,
	.chown = myfs_chown,
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
		memset(&root_dir_contents, 0, sizeof(root_dir_contents));

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

//fetch a directory data structure from the database at the given key and load
//it into the given buffer
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

//store the directory data structure passed given at the given key
int storeDirectoryDataFromUnqliteStore(uuid_t *key_id, dir_data *value_addr){

		//store the directory data at the given address and record the result code
		int rc = unqlite_kv_store(pDb,*key_id,KEY_SIZE,value_addr,sizeof(dir_data));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
		}
}

//fetch a regular file structure from the database at the given key and load
//it into the given buffer
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

//store the regular file structure structure given at the given key
int storeRegularFileDataFromUnqliteStore(uuid_t *key_id, reg_data *value_addr){

		//store the regulat filed data structure at the given address and record the result code
		int rc = unqlite_kv_store(pDb,*key_id,KEY_SIZE,value_addr,sizeof(reg_data));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
		}
}

//fetch a single indirect block structure from the database at the given key and load
//it into the given buffer
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

//store the single indirect structure structure given at the given key
int storeSingleIndirectBlockFromUnqliteStore(uuid_t *key_id, single_indirect *value_addr){

		//store the single indirect at the given address and record the result code
		int rc = unqlite_kv_store(pDb,*key_id,KEY_SIZE,value_addr,sizeof(single_indirect));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
		}
}

//fetch a single data block structure from the database at the given key and load
//it into the given buffer
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

//store the single data block structure given at the given key
int storeDataBlockFromUnqliteStore(uuid_t *key_id, data_block *value_addr){

		//store the data block at the given address and record the result code
		int rc = unqlite_kv_store(pDb,*key_id,KEY_SIZE,value_addr,sizeof(data_block));
		
		if( rc != UNQLITE_OK ){
   			error_handler(rc);
		}
}

//fill the given stat struct with the data from the file control block
int fillStatWithFileNode(struct stat* destination, file_node* source) {

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

//create a new file control block for the file at the given path by filling
//the file control block buffer
int initNewFCB(const char* path, file_node* buff){

	//reset the space for the new file control block
	memset(buff, 0, sizeof(file_node));

	//copy the path into the file node path
	strcpy(buff->path, path);

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

//retrieve the file control block of the file at the given path and also
//the uuid of where that file control block is being kept in the database
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

			write_log("Locating file node failed : %s is not a directory.\n", pathSeg);
			printf("%s is not a directory.\n", current_node.path);
			return -ENOENT;

		} else {

			// write_log("%s fetching from unqlite store\n", current_node.path);

			//fetch the current directory data
			memset(&current_dir_data, 0, sizeof(dir_data));

			// write_log("memset directory data successful\n");

			if(fetchDirectoryDataFromUnqliteStore(&current_node.data_id, &current_dir_data) != 1) {
				write_log("file not found\n");
				return -ENOENT;
			}

			// write_log("fetch successful for %s\n", current_node.path);

			if(IS_ROOT(current_node.path))
				numberOfEntries = current_node.size - 2;
			else
				numberOfEntries = current_node.size;
			

			// write_log("number of entries : %d\n", numberOfEntries);

			//get the list of entries from the current directory data
			dir_entry curr_entries[numberOfEntries];
			memset(curr_entries, 0, sizeof(dir_entry) * numberOfEntries);
			memcpy(&curr_entries, &current_dir_data.entries, sizeof(dir_entry) * numberOfEntries);

			// write_log("set entries array\n");

			//while there are still directory entries left and the directory
			//that we need has not been found
			for(int i = 0; i < numberOfEntries; i++) {

				//get the next entry in the collection of entries
				memset(&curr_entry, 0, sizeof(dir_entry));
				curr_entry = curr_entries[i];

				// write_log("\n\nPATH : %s\nSegment : %s\n\n", curr_entry.path, pathSeg);

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
	else {
		// write_log("get file node returns error\n");
		return -ENOENT;
	}

	// write_log("returning successfully on getting file node");
	return 0;
}

//retrieve the file control block of the parent directory of the file
//at the given path and also the uuid of where that file control block
//is being kept in the database
int getParentFileNode(const char* path, file_node *buffer, uuid_t* buff_uuid){

	//get the length of the path so that we can copy the path over
	int lengthOfPath = strlen(path);

	//the place for the copy of the patj so that we can extract the parent directory
	//from the file path
	char pathBuff[lengthOfPath];

	//copy the const char* path into the pathBuff array
	sprintf(pathBuff, path);

	//the parent path is extract from the path buff using the dirname function, which
	//strips of the final path segment ("man 3 dirname")
	char* parentPath = dirname(pathBuff);

	//get the file control block and the uuid of the parent
	getFileNode(parentPath, buffer, buff_uuid);

	return 0;

}

//create a direct entry to link the file name with the given parent node and return it
//in the directory entry buffer and store the directory entry in the given directory data
int makeDirent(char* filename, file_node *parentNode, dir_entry *dirent, dir_data *dirdata) {

	//copy the file name into the path of the directory entry so that we know that
	//the uuid stored in the directory entry is where the file with that file name is
	//kept in the file store
	sprintf(dirent->path, filename);

	//generate a uuid for the directory entry
	uuid_generate(dirent->fileNodeId);

	//fetch the directory data of the parent directory from the store
	fetchDirectoryDataFromUnqliteStore(&parentNode->data_id, dirdata);

	//the index of where the new entry will go in the array of directory entries
	//in the directory data
	int new_entry_position, freeSpace;

	//if the parent is the root then set the new entry position without consideration
	//for "." and ".." since we have not stored these in the root directory, and increment
	//the size of the root file control block. Otherwise set the new entry position as the
	//size of the parent node
	if(IS_ROOT(parentNode->path)) {
		if((freeSpace = freeSpaceInList(dirdata)) == -1) 
			new_entry_position = parentNode->size - 2;
		else
			new_entry_position = freeSpace - 2;
		the_root_fcb.size++;
	} else {
		if((freeSpace = freeSpaceInList(dirdata)) == -1) 
			new_entry_position = parentNode->size;
		else
			new_entry_position = freeSpace;
		new_entry_position = parentNode->size;
	}

	//copy the data stored in the directory entry that we have created into
	//the directory data at the new entry index
	memcpy(&dirdata->entries[new_entry_position], dirent, sizeof(dir_entry));

	//increment the size kept by the parent node
	parentNode->size++;

	return 0;

}

int makeCWDdirent(uuid_t *cwdId, dir_data *newDirData){

	dir_entry cwdDirent;
	memset(&cwdDirent, 0, sizeof(dir_entry));

	uuid_copy(cwdDirent.fileNodeId, *cwdId);
	sprintf(cwdDirent.path, CWD_STR);

	memcpy(&newDirData->entries[0], &cwdDirent, sizeof(dir_entry));

	return 0;

}

int makePDdirent(uuid_t *pdId, dir_data *newDirData){

	dir_entry pdDirent;
	memset(&pdDirent, 0, sizeof(dir_entry));

	uuid_copy(pdDirent.fileNodeId, *pdId);
	sprintf(pdDirent.path, PD_STR);

	memcpy(&newDirData->entries[1], &pdDirent, sizeof(dir_entry));

	return 0;
}

//get the offsets needed for the indirect block, direct block and interior character in the data buffer from
//the offset value that has been given
int getOffsets(off_t offset, int *indir_block_offset, int *small_block_offset, int *interior_block_offset) {

	//if the offset starts after the end of the direct blocks
	if(offset > MY_DIRECT_BLOCKS_SIZE) {
		//determines which indirect block we need
		*indir_block_offset = (offset - MY_DIRECT_BLOCKS_SIZE) / MY_SINGLE_INDIRECT_BLOCK_SIZE;

		//determines which block in the direct block we start
		*small_block_offset = (offset - MY_DIRECT_BLOCKS_SIZE - (*indir_block_offset * MY_SINGLE_INDIRECT_BLOCK_SIZE)) / MY_BLOCK_SIZE;

		//where in the block of memory we start writing
		*interior_block_offset = (offset - MY_DIRECT_BLOCKS_SIZE - (*indir_block_offset * MY_SINGLE_INDIRECT_BLOCK_SIZE) - (*small_block_offset * MY_BLOCK_SIZE));

	} else {

		//no indirect blocks yet so we set this to -1
		*indir_block_offset = -1;

		//determines which direct block we are starting from
		*small_block_offset = offset / MY_BLOCK_SIZE;

		//get where we need to start inside of the data block
		*interior_block_offset = offset - (MY_BLOCK_SIZE * *small_block_offset);

	}

	return 0;
}

//get the size of a memory block to read depending on the offsets and the remaining size to be read
int getSizeToRead(size_t *size_to_read, size_t *remaining_size, int *interior_block_offset) {

	//if the remaining size left will not fill the block
	if(*remaining_size < (MY_BLOCK_SIZE - *interior_block_offset)) {
		
		//we write the remaining size to the block
		*size_to_read = *remaining_size;
		*remaining_size = 0;

	} else {

		//set the size to write as the size of the rest of the block
		//from the interior block offset
		*size_to_read = MY_BLOCK_SIZE - *interior_block_offset;

		//decrease the remaining size by the amount we need to write
		*remaining_size -= *size_to_read;
	}

	return 0;

}

//reads a block at the given source uuid and copies it into the destination uuid. If
//it does not exist then we return -1 so that the read function knows to return the
//number of bytes that have been read at this point since there is nothing left
int getReadDataBlockData(data_block *curr_block, uuid_t *dest, uuid_t *src){

	//get the uuid of the next direct block
	uuid_copy(*dest, *src);

	//if the block has not been created and so has a blank uuid then create a new uuid for it
	if(UUID_IS_BLANK(*dest)) {

		write_log("myfs_read data block uuid not found");
		return -1;

	} else {

		//get the data block from the file
		fetchDataBlockFromUnqliteStore(dest, curr_block);

	}

	return 0;
}

//reads a block at the given source uuid and copies it into the destination uuid. If
//it does not exist then we generate a new uuid for the block
int getWriteDataBlockData(data_block *curr_block, uuid_t *dest, uuid_t *src){

		//get the uuid of the next direct block
		uuid_copy(*dest, *src);

		//if the block has not been created and so has a blank uuid then create a new uuid for it
		if(UUID_IS_BLANK(*dest)) {

			uuid_generate(*src);
			uuid_copy(*dest, *src);

		} else {

			//get the data block from the file
			fetchDataBlockFromUnqliteStore(dest, curr_block);

		}

		return 0;

}

//reads a single indirect block at the given source uuid and copies it into the destination uuid. If
//it does not exist then we return -1 so that the read function knows to return the
//number of bytes that have been read at this point since there is nothing left
int getReadSingleIndirectBlock(single_indirect *indir_block, uuid_t *dest, uuid_t *src) {

	//reset the block
	memset(indir_block, 0, sizeof(single_indirect));

	//try to copy the uuid of the block into the general single indirect block
	uuid_copy(*dest, *src);

	//if the uuid is blank then that means there is no more data to read and we return
	//how much we have read
	if(UUID_IS_BLANK(*dest)) {

		write_log("myfs_read indirect block uuid not found");
		return -1;

	//otherwise, make a database call for the next indirect block
	} else
		fetchSingleIndirectBlockFromUnqliteStore(dest, indir_block);

	return 0;
}

//reads a single indirect block at the given source uuid and copies it into the destination uuid. If
//it does not exist then we generate a new uuid for the block
int getWriteSingleIndirectBlock(single_indirect *indir_block, uuid_t *dest, uuid_t *src) {
	
	//reset the block
	memset(indir_block, 0, sizeof(single_indirect));

	//try to copy the uuid of the block into the general single indirect block
	uuid_copy(*dest, *src);

	//if the uuid is blank then create a new uuid for it and don't try to do any database fetches
	if(UUID_IS_BLANK(*dest)) {
		uuid_generate(*src);
		uuid_copy(*dest, *src);

	//if the uuid is not blank then make the copy from the database with the given uuid
	} else
		fetchSingleIndirectBlockFromUnqliteStore(dest, indir_block);

	return 0;

}

//removes the directory entry of the file at the given path from the parent node.
int removeDirent(const char* path, file_node *parent_node){

	//the return code from the database call,
	//a stepper for the search for the file,
	//the limit for how many non empty items we have found
	//the size of the path of the file we are deleting,
	//the size we are checking
	//and the flag for when we find the entry to delete
	int rc, stepper = 0, limit = 0, path_size = strlen(path), size, entryFound = 0;

	//buff to load the path into so that we can
	//perform the basename function on the path
	char pathBuff[path_size];

	//the directory data of the parent directory
	dir_data parent_data;

	//the current directory entry that we are checking
	dir_entry curr_entry;

	//copy the const char* path into the pathBuff array
	sprintf(pathBuff, path);

	//the name of the file, which is how the file at the given path is represented
	//in the directory
	char* filename = basename(pathBuff);

	//reset all of the bits of the directory data structure and then
	//fill that space with the directory data fetched from the database
	memset(&parent_data, 0, sizeof(dir_data));

	//try to get the directory data of the parent node from the UnQlite store
	if((rc = fetchDirectoryDataFromUnqliteStore(&parent_node->data_id, &parent_data)) != 0)
		return rc;

	//get the size needed for the search
	if(IS_ROOT(parent_node->path))
		size = parent_node->size - 2;
	else
		size = parent_node->size;

	//reset the curr_entry
	memset(&curr_entry, 0, sizeof(dir_entry));

	//while there are still entries in the parent directory data
	while(limit < size && entryFound == 0) {

		//the current entry is at the stepper location in the directory data's
		//entries list
		curr_entry = parent_data.entries[stepper];

		//if the path of the entry we are checking matches the name
		//of the file we are deleting then exit the loop. Otherwise,
		//keep searching
		if(strcmp(curr_entry.path, filename) == 0)
			entryFound = 1;
		//if the entry is not in the free list then we can increase the limit counter
		//as we know it means that the slot that we checked is not empty
		else if(entryIsInFreeList(&parent_data, &stepper) == -1)
			limit++;

		stepper++;
	}

	//if an entry was found then delete it
	if(entryFound == 1) {
		parent_data.free_slots[parent_data.numberOfFreeSlots++] = stepper;
		memset(&parent_data.entries[stepper], 0, sizeof(dir_entry));
	//otherwise return ENOENT
	} else {
		write_log("failed to delete %s : entry not found in parent directory\n", path);
		return -ENOENT;
	}


	return 0;

}

//check whether the given index is in the free list or not
int entryIsInFreeList(dir_data *parent_data, int *entryToSearchFor) {

	int stepper = 0, entryFound = -1, currentEntry = 0, limit = 0;

	while(limit < parent_data->numberOfFreeSlots && entryFound == 0) {
		if((currentEntry = parent_data->free_slots[stepper]) == *entryToSearchFor)
			entryFound = 1;
		else if(currentEntry != -1) {
			limit++;
		}
		stepper++;
	}

	return entryFound;
}

//checks if there is a free space in the free list and returns it. Otherwise, returns -1
int freeSpaceInList(dir_data *directData) {

	int stepper = 0, slotFound = 0, currentSlot,  result = 0;

	if(directData->numberOfFreeSlots == 0) {

		return -1;

	} else {

		while(stepper < directData->numberOfFreeSlots && slotFound == 0) {

		if((currentSlot = directData->free_slots[stepper]) != -1) {
			slotFound = 1;
			result = currentSlot;
			directData->free_slots[stepper] = -1;
			directData->numberOfFreeSlots--;
		}

		stepper++;

		}

	}

	return result;
}