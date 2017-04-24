# User Level File System
An implementation of a user-level file system using the FUSE library in C. This was the second practical in the operating systems
module in 3rd year. It uses UnQLite for storage and holds file information using a file control block structure similar to Unix inodes.
The system uses indexed allocation over the UnQLite storage for the file data blocks. The majority of code is code provided by 
the University of St Andrews, except the code in myfs.c and myfs.h. All of my code is kept in two files, as this was a restriction
for the practical requirements.
