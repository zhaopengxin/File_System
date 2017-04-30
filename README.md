# Network File Server

### Overview

In this project, we implemented a multi-threaded, secure network file server. Clients that use our file server can interact with it via network messages(TCP). Involved knowledge of this project are hierarchical file systems, socket programming, client-server systems, and security protocols.

Our file server provides a hierarchical file system. Files and directories stored on the file server are referred to by a full pathname, with / being the delimiting character. For
example, the pathname /usr/pengxin/doc refers to a file doc that is stored in the directory /usr/pengxin. Pathnames must start with /, and they must not end with /.
Directories store files and/or sub-directories; files store data. Each file and directory is owned by a particular user, except for the root directory /, which is owned by all users.

Users may only access files and directories they own.
