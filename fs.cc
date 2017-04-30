#include "fs_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <iostream>
#include <sstream>
#include <thread>
#include <string>
#include <regex>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <cassert>

using namespace std;

static const unsigned int BLOCK_NUMBER = FS_DISKSIZE/FS_BLOCKSIZE;
static const unsigned int MAXSIZE_INT = 10;

/* Data structures */

// session id, sequence, username, password and lock
static unsigned int session_id = 0;                                    // increase by one
static bool session_max = false;                                       // Set if num of session reaches the maximum
static unordered_map <unsigned int, unsigned int>            SS_map;   // session id -> largest sequence id received 
static unordered_map <string, unordered_set<unsigned int> >  US_map;   // username -> session id
static unordered_map <string, string>                        UP_map;   // username -> password
mutex ssmap_lock;                                                      // lock for US_map and SS_map

// In-memory free block list and lock
static unsigned int num_block_remain;
static deque<unsigned int> free_blocks;
mutex free_blocks_lock;

// Read-write lock for every file server entity
struct rw_mutex_t {
    pthread_mutex_t *mtx;
    pthread_cond_t *waiting_readers;
    pthread_cond_t *waiting_writers;
    unsigned int reading_num = 0;
    unsigned int writing_num = 0;
    rw_mutex_t() {
        mtx = new pthread_mutex_t();
        waiting_readers = new pthread_cond_t();
        waiting_writers = new pthread_cond_t();
        if (pthread_mutex_init(mtx, nullptr)) {
            cerr << "mutex fail" << endl;
        }
        if (pthread_cond_init(waiting_readers, nullptr)) {
            cerr << "cv fail" << endl;
        }
        if (pthread_cond_init(waiting_writers, nullptr)) {
            cerr << "cv fail" << endl;
        }
    }
    ~rw_mutex_t() {
        pthread_mutex_destroy(mtx);
        pthread_cond_destroy(waiting_readers);
        pthread_cond_destroy(waiting_writers);
        delete mtx;
        delete waiting_readers;
        delete waiting_writers;
    }
    void read_lock() {
        pthread_mutex_lock(mtx);
        while(writing_num > 0){
            pthread_cond_wait(waiting_readers, mtx);
        }
        reading_num++;
        pthread_mutex_unlock(mtx);
    } 
    void read_unlock() {
        pthread_mutex_lock(mtx);
        reading_num--;
        if (reading_num == 0) {
            pthread_cond_signal(waiting_writers);
        }
        pthread_mutex_unlock(mtx);
    }
    void write_lock() {
        pthread_mutex_lock(mtx);
        while(reading_num + writing_num > 0){
            pthread_cond_wait(waiting_writers, mtx);
        }
        writing_num++;
        pthread_mutex_unlock(mtx);
    }
    void write_unlock() {
        pthread_mutex_lock(mtx);
        writing_num--;
        pthread_cond_broadcast(waiting_readers);
        pthread_cond_signal(waiting_writers);
        pthread_mutex_unlock(mtx);
    }
};

// A rw-lock manager for the entire file server. 
// The lock of a particular fs entity can be manipulated 
// by passing in the inode number as parameter.
struct mm_fs_locks_t {
    unordered_map<unsigned int, rw_mutex_t*> fs_locks; 
    mutex rwmap_lock;

    void add_lock(unsigned int inode) {
        rwmap_lock.lock(); 
        fs_locks[inode] = new rw_mutex_t();
        rwmap_lock.unlock(); 
    }
    void delete_lock(unsigned int inode) {
        rwmap_lock.lock(); 
        delete fs_locks[inode];
        fs_locks.erase(inode); 
        rwmap_lock.unlock(); 
    }
    void r_lock(unsigned int inode) {
        rwmap_lock.lock(); 
        rw_mutex_t* lock = fs_locks[inode];
        rwmap_lock.unlock(); 
        lock->read_lock();
    }
    void w_lock(unsigned int inode) {
        rwmap_lock.lock(); 
        rw_mutex_t* lock = fs_locks[inode];
        rwmap_lock.unlock(); 
        lock->write_lock();
    }
    void r_unlock(unsigned int inode) {
        rwmap_lock.lock(); 
        rw_mutex_t* lock = fs_locks[inode];
        rwmap_lock.unlock(); 
        lock->read_unlock();
    }
    void w_unlock(unsigned int inode) {
        rwmap_lock.lock(); 
        rw_mutex_t* lock = fs_locks[inode];
        rwmap_lock.unlock(); 
        lock->write_unlock();
    }
};
static mm_fs_locks_t mm_fs_locks;

// Request: header+body+type
struct request_t {
    string header;
    char *request_body;
};
enum request_type { SESSION, READ, WRITE, CREATE, DELETE, INVALID };


/* Disk operations */

// Traverse the path and conduct the corresponding operation on file system and disk.
static bool conduct_operation(const string &path, const char* username, unsigned int offset,
                              const char cr_type, const void* write_data, void* read_data,
                              request_type rtype) {
    if (rtype == SESSION) { return true; }
    
    // divide the path into tokens
    vector<string> paths;
    // '/' root directory is not valid here, get all tokens 
    unsigned int i = 0, j;
    unsigned int n = path.size();
    string path_str(path);
    /*
        error handling:
        1. Empty path name
        2. The last character is '/'
        3. The path isn't start from root directory
    */
    if (path_str.empty()) { return false; }
    if (path_str[path_str.size()-1] == '/') { return false; }
    if (path_str[0] != '/')  { return false; }

    // parse the path name to file(dir)names
    while (i < n) {
        j = i+1;
        while (j < n && path_str.at(j) != '/' && path_str.at(j) != '\0') {
            j++;
        }
        if (j == i+1) { return false; }
        if (j-i-1 > FS_MAXFILENAME) { return false; }
        paths.push_back(path_str.substr(i+1, j-i-1));
        i = j;
    }

    // find inode pointing to aimed block
    int inode_block = 0, inode_tmp;
    int dir_block;
    fs_inode *inode_buf = new fs_inode();
    fs_direntry *dirs_buf = new fs_direntry[FS_DIRENTRIES];
    
    //if the operation is CREATE or DELETE, then reserve the former directory block
    unsigned int path_depth = ((rtype == CREATE) || (rtype == DELETE))? paths.size()-1: paths.size();
    if (path_depth == 0 && (rtype == DELETE || rtype == CREATE)) {              
        mm_fs_locks.w_lock(inode_block);
    } else {
        mm_fs_locks.r_lock(inode_block);
    }
    
    //Traverse the path, find the target inode and hold the relatived lock
    for (i = 0; i < path_depth; i++) {
        disk_readblock(inode_block, (void*)inode_buf);
        char type = inode_buf->type; 
        
        // directory type check
        if (type != 'd') { 
            delete inode_buf;
            delete [] dirs_buf;
            mm_fs_locks.r_unlock(inode_block);
            return false; 
        }
        // owners check
        const char* owners = inode_buf->owner;
        if (strcmp(owners, username) != 0 && strcmp("", owners) != 0) {
            delete inode_buf;
            delete [] dirs_buf;
            mm_fs_locks.r_unlock(inode_block);
            return false;
        }
        uint32_t size = inode_buf->size;
        // validate filename
        bool find = false;
        for (j = 0; j < size; j++) {
            uint32_t blocknum = inode_buf->blocks[j];
            dir_block = blocknum;
            disk_readblock(dir_block, (void*)dirs_buf);
            // one block 
            for (unsigned int k = 0; k < FS_DIRENTRIES; k++) {
                const char* name = dirs_buf[k].name;
                if (strcmp(name, paths[i].c_str()) == 0) {
                    inode_tmp = dirs_buf[k].inode_block;
                    find = true;
                    break;
                }
            }
            if (find == true) {
                // hand-over-hand
                if (path_depth == i+1 && (rtype != READ)) {
                    mm_fs_locks.w_lock(inode_tmp);
                } else {
                    mm_fs_locks.r_lock(inode_tmp);
                }
                mm_fs_locks.r_unlock(inode_block);
                inode_block = inode_tmp;
                break;
            }
        }
        if (find == false) {
            delete inode_buf;
            delete [] dirs_buf;
            mm_fs_locks.r_unlock(inode_block);
            return false;
        }
    }

    delete [] dirs_buf;

    // judge whether the finding inode is valid
    disk_readblock(inode_block, (void*)inode_buf);

    // check owners
    const char* owners = inode_buf->owner;
    if (strcmp(owners, username)!=0 && strcmp("", owners)!=0) {
        if (rtype != READ) {                        
            mm_fs_locks.w_unlock(inode_block);
        } else {
            mm_fs_locks.r_unlock(inode_block);
        }
        delete inode_buf;
        return false;
    }

    // Now: we have inode lock, and inode is owned by me. 
    // Do error handling work according to request type.
    fs_inode* inode = inode_buf;
    bool error = false;
    switch (rtype) {
        // READ: check inode type and read block offset
        case READ: {
            unsigned int block_idx;
            if (inode->type != 'f') {
                // not a file
                error = true;
            }
            if (offset >= inode->size) {
                // offset out of range
                error = true;
            }
            if (error) {
                delete inode;
                mm_fs_locks.r_unlock(inode_block);
                return false;
            }

            block_idx = inode->blocks[offset];
            
            // read the data
            disk_readblock(block_idx, read_data);

            mm_fs_locks.r_unlock(inode_block);
            break;
        }
        
        // WRITE: check inode type, write block offset, free block 
        case WRITE: {
            unsigned int block_idx;

            // error handling
            if (inode->type != 'f') {
                // not a file
                error = true;
            }
            if (offset > inode->size) {
                // offset out of range
                error = true;
            }
            if (offset >= FS_MAXFILEBLOCKS) {
                // file is out of space
                error = true;
            }
            if (error) {
                delete inode;
                mm_fs_locks.w_unlock(inode_block);
                return false;
            }

            if (offset == inode->size) {
                //need to allocate new block
                free_blocks_lock.lock();
                if (num_block_remain < 1) {
                    // disk is out of space
                    delete inode;
                    free_blocks_lock.unlock();
                    mm_fs_locks.w_unlock(inode_block);
                    return false;
                }

                num_block_remain--;
                block_idx = free_blocks.front();
                free_blocks.pop_front();
                free_blocks_lock.unlock();
            } else {
                block_idx = inode->blocks[offset];
            }

            // write file block
            disk_writeblock(block_idx, write_data);

            // modify inode
            if (offset == inode->size) {
                inode->size++;
                inode->blocks[offset] = block_idx;
                disk_writeblock(inode_block, (void*)inode);            
            }

            mm_fs_locks.w_unlock(inode_block);
            break;
        }
        
        // CREATE: check inode type, free block, duplicate creation
        case CREATE: {
            const char* name = paths.back().c_str();

            // invalid create type;
            if (cr_type != 'f' && cr_type != 'd') {
                delete inode;
                mm_fs_locks.w_unlock(inode_block);
                return false;
            }

            // not a directory
            if (inode->type != 'd') {
                delete inode;
                mm_fs_locks.w_unlock(inode_block);
                return false;
            }
            
            // find an empty dir-entry in current size of inode
            bool diret_found = false;
            unsigned int block_num = 0, dir_num = 0;
            fs_direntry *tmp_direts, *fd_direts;
            tmp_direts = new fs_direntry[FS_DIRENTRIES];
            fd_direts  = new fs_direntry[FS_DIRENTRIES];
            for (unsigned int i = 0; i < inode->size; i++) {
                unsigned int block_idx = inode->blocks[i];
                disk_readblock(block_idx, tmp_direts);
                for (unsigned int j = 0; j < FS_DIRENTRIES; j++) {
                    if (tmp_direts[j].inode_block == 0 && !diret_found) { 
                        diret_found = true;
                        block_num = i;
                        dir_num = j;
                        memcpy(fd_direts, tmp_direts, sizeof(fs_direntry)*FS_DIRENTRIES);
                    }
                    if ((tmp_direts[j].inode_block != 0) && (strcmp(tmp_direts[j].name, name) == 0)) {
                        delete [] tmp_direts;
                        delete [] fd_direts;
                        delete inode;
                        mm_fs_locks.w_unlock(inode_block);
                        return false;
                    }
                }
            }
            delete [] tmp_direts;

            // error handling
            if (!diret_found && inode->size == FS_MAXFILEBLOCKS) {
                delete inode;
                delete [] fd_direts;
                mm_fs_locks.w_unlock(inode_block);
                return false;
            }

            free_blocks_lock.lock();
            if (diret_found) {
                // need 1 free block
                if (num_block_remain < 1) {
                    error = true;
                } else {
                    num_block_remain--;
                }
            } else {
                // need 2 free block
                if (num_block_remain < 2) {
                    error = true;
                } else {
                    num_block_remain -= 2;
                }
            }
            free_blocks_lock.unlock();

            if (error) {
                delete inode;
                delete [] fd_direts;
                mm_fs_locks.w_unlock(inode_block);
                return false;
            }
                
            // create new inode
            free_blocks_lock.lock();
            unsigned int inode_idx = free_blocks.front();
            free_blocks.pop_front();
            free_blocks_lock.unlock();
            fs_inode new_inode;

            new_inode.type = cr_type;
            new_inode.size = 0;
            strcpy(new_inode.owner, username);

            disk_writeblock(inode_idx, (void*)(&new_inode));
            mm_fs_locks.add_lock(inode_idx);

            // create direntry in directory
            if (!diret_found) {
                free_blocks_lock.lock();
                int block = free_blocks.front();
                free_blocks.pop_front();
                free_blocks_lock.unlock();
                dir_num = 0;
                for (unsigned int i = 0; i < FS_DIRENTRIES; i++) {
                    fd_direts[i].inode_block = 0;
                }
                inode->blocks[inode->size++] = block;
                block_num = inode->size-1;
            } 

            strcpy(fd_direts[dir_num].name, name);
            fd_direts[dir_num].inode_block = inode_idx;
            disk_writeblock(inode->blocks[block_num], (void*)fd_direts);

            if (!diret_found) {
                disk_writeblock(inode_block, (void*)inode);
            }

            delete [] fd_direts;
            mm_fs_locks.w_unlock(inode_block);
            break;
        }
        
        // DELETE: inode type, target exists, owner, directory is deletable
        case DELETE: {
            const char* name = paths.back().c_str();

            // not a directory
            if (inode->type != 'd') {
                delete inode;
                mm_fs_locks.w_unlock(inode_block);
                return false;
            }

            // find the direntry point to the directory or file
            bool diret_found = false;
            unsigned int block_num = 0, dir_num = 0;
            fs_direntry *tmp_direts, *fd_direts;
            tmp_direts = new fs_direntry[FS_DIRENTRIES];
            fd_direts  = new fs_direntry[FS_DIRENTRIES];
            for (unsigned int i = 0; i < inode->size; i++) {
                unsigned int block_idx = inode->blocks[i];
                disk_readblock(block_idx, tmp_direts);
                for (unsigned int j = 0; j < FS_DIRENTRIES; j++) {
                    if (tmp_direts[j].inode_block == 0) continue; 
                    if (strcmp(tmp_direts[j].name, name) != 0) continue;
                    diret_found = true;
                    block_num = i;
                    dir_num = j;
                    memcpy(fd_direts, tmp_direts, sizeof(fs_direntry)*FS_DIRENTRIES);
                    break;
                }
                if (diret_found) {
                    break;
                }
            }
            delete [] tmp_direts;

            if (!diret_found) { 
                // pathname not exist
                delete inode;
                mm_fs_locks.w_unlock(inode_block);
                return false;
            }

            unsigned int inode_del_idx = fd_direts[dir_num].inode_block;
            fs_inode inode_del;

            mm_fs_locks.w_lock(inode_del_idx);
            disk_readblock(inode_del_idx, (void*)(&inode_del));
            if (inode_del.type == 'd' && inode_del.size > 0) {
                // directory not empty
                error = true;
            }
            if (strcmp(inode_del.owner, username)) {
                // owner not right
                error = true;
            }
            if (error) {
                delete inode;
                delete [] fd_direts;
                mm_fs_locks.w_unlock(inode_del_idx);
                mm_fs_locks.w_unlock(inode_block);
                return false;
            }
          
            // modify the directory
            unsigned int count = 0;
            for (unsigned int i = 0; i < FS_DIRENTRIES; i++) {
                if (fd_direts[i].inode_block != 0) {
                    count++;
                }
            }
            bool empty = (count == 1);
            if (empty) {
                // modify inode
                // shift following blocks in inode
                for (unsigned int i = block_num; i+1 < inode->size; i++) {
                    inode->blocks[i] = inode->blocks[i+1];
                }
                inode->size--;
                disk_writeblock(inode_block, (void*)inode);

                // free the direntry block
                free_blocks_lock.lock();
                num_block_remain++;
                free_blocks.push_back(inode->blocks[block_num]);
                free_blocks_lock.unlock();
            } else {
                // modify direntry
                fd_direts[dir_num].inode_block = 0;
                fd_direts[dir_num].name[0] = '\0';
                disk_writeblock(inode->blocks[block_num], (void*)fd_direts);
            }
                
            // clear the blocks of dir or file
            if (inode_del.type == 'f') {
                // all the file blocks
                int size = inode_del.size;
                inode_del.size = 0;
                for (int i = 0; i < size; i++) {
                    free_blocks_lock.lock();
                    num_block_remain++;
                    free_blocks.push_back(inode_del.blocks[i]);
                    free_blocks_lock.unlock();
                }
            }
            free_blocks_lock.lock();
            num_block_remain++;
            free_blocks.push_back(inode_del_idx);
            free_blocks_lock.unlock();

            delete [] fd_direts;
            mm_fs_locks.w_unlock(inode_del_idx);
            mm_fs_locks.delete_lock(inode_del_idx);
            mm_fs_locks.w_unlock(inode_block);
            break;
        }

        default: { break; }
    } 
    delete inode;
    return true;
}


/* utility functions */

// count the number of spaces in a c_string
static unsigned int count_spaces(const char* str) {
    int c = 0;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == ' ') {
            c++;
        }
    }
    return c;
}

// Transfer a c-string to an unsigned integer.
// Return true if success, false if failed
static bool cvt_int(const char* str, int size, unsigned int &num) {
    if (size==0) { return false; }
    long long long_int = 0;
    for (int i = 0; i < size; i++) {
        if (!isdigit(str[i])) { return false; }
        long_int = long_int*10 + str[i]-'0';
    } 
    if (size > 1 && str[0] == '0') { return false; }
    if (long_int > numeric_limits<unsigned int>::max()) { return false; }
    num = (unsigned int)long_int;
    return true;
}

// Send response message to the client, it will not be called if the request was invalid
// READ request: <session> <sequence><NULL><data>
// Other request: <session> <sequence><NULL>
static void send_response(request_type type, size_t session_id, size_t sequence, 
                         void* rd_data, int socket, const char* password) {
    char *header, *cleartext, *ciphertext;
    unsigned int size_cleartext, size_ciphertext;

    string tmp(to_string(session_id)+" "+to_string(sequence));
    unsigned int tmp_size = tmp.size();

    // get the clear text
    if (type != READ) {
        // only session + sequence
        cleartext = new char[tmp_size+1];
        strcpy(cleartext, tmp.c_str());
        size_cleartext = strlen(cleartext)+1;
    } else {
        // session + sequence + data
        cleartext = new char[tmp_size+1+FS_BLOCKSIZE];
        strcpy(cleartext, tmp.c_str());
        for (unsigned int i = 0; i < FS_BLOCKSIZE; i++) {
            cleartext[tmp_size+1+i] = ((char*)rd_data)[i];  
        }
        size_cleartext = tmp_size+1+FS_BLOCKSIZE;  
    }

    // ciphertext and header
    ciphertext = (char*)fs_encrypt(password, cleartext, size_cleartext, &size_ciphertext);
    string s = to_string(size_ciphertext);
    header = new char[s.size()+1];
    strcpy(header, to_string(size_ciphertext).c_str());

    // send header and ciphertext
    unsigned int sent = 0;
    while (sent < strlen(header)+1) {
        int s = send(socket, header+sent, strlen(header)+1-sent, MSG_NOSIGNAL);
        sent += s;
    }

    sent = 0;
    while (sent < size_ciphertext) {
        int s = send(socket, ciphertext+sent, size_ciphertext-sent, MSG_NOSIGNAL);
        sent += s;
    }

    // delete allocated memory
    delete [] ciphertext;
    delete [] header;
    delete [] cleartext;
}

// Parse the request body after we get valid username, password, session and sequence number 
// Get pathname, block number, create type (file/dir), and writing data 
static bool parse_req(const char* req_begin, request_type type, 
                  unsigned int size,
                  string &pathname, unsigned int &block, 
                  char &cr_type, char* data) {
    if (size == 0) { return false; }

    if (type == SESSION) {
        if (size == 1 && req_begin[0] == '\0') {
            return true;
        } else {
            return false;
        }
     }

    unsigned int l = 0, r = l;
    if (req_begin[r] != ' ') { return false; }
    l = r+1;
    size -= 1;
    if (size == 0) { return false; }

    // pathname
    for (r = l; (r-l+1) < size && req_begin[r] != ' '; r++);
    for (unsigned int i = 0; i < r-l; i++) {
       if (isspace(req_begin[l+i])) { return false; }
       pathname += req_begin[l+i]; 
    }
    if (r-l > FS_MAXPATHNAME) { return false; }
    size -= (r-l+1);
    l = r+1;

    switch (type) {
        case READ: {
            // block
            if (size == 0) { return false; }
            for (r = l; (r-l+1) < size && req_begin[r] != '\0'; r++);
            if (!cvt_int(req_begin+l, r-l, block)) { return false; }
            if (block >= FS_MAXFILEBLOCKS) { return false; }
            size -= (r-l+1);
            if (req_begin[r] == '\0' && size == 0) {
                return true; 
            } else {
                return false;
            }
        } 

        case WRITE: {
            // block
            if (size == 0) { return false; }
            for (r = l; (r-l+1) < size && req_begin[r] != '\0'; r++);
            if (!cvt_int(req_begin+l, r-l, block)) { return false; }
            if (block >= FS_MAXFILEBLOCKS) { return false; }

            size -= (r-l+1);
            l = r+1;
            if (size != FS_BLOCKSIZE) { return false; }
            
            // data
            for (r = l; (r-l) < size; r++) {
                data[r-l] = req_begin[r];
            } 
            size -= (r-l);
            if (size != 0 ) { return false; }
            return true;
        }

        case CREATE: {
            if (size != 2) { return false; }
            cr_type = req_begin[l];
            if ((cr_type != 'd') && (cr_type != 'f')) { return false; }
            if (req_begin[l+1] != '\0') { return false; }
            free_blocks_lock.lock();
            if (num_block_remain <= 0) { 
                free_blocks_lock.unlock();
                return false;
            } 
            free_blocks_lock.unlock();
            return true; 
        }

        case DELETE:  {
            if (size == 0 && req_begin[r] == '\0') {
                return true;
            } else {
                return false;
            }
        }
        default: { break; }
    }
    return true;
};

// Handle the request message from client. Return immediately if the request is recognized
// as invalid.
// 1. Decrypt the request body
// 2. Parse the request body
// 3. Conduct the operations
// 4. Send response
static void message_handler(request_t *request, int socket) {
    string header(request->header);
    string body(request->request_body);
    if (count_spaces(header.c_str()) != 1) return;
    int pos = header.find(" ");

    // error handling(EH): 
    // 1. username is unknown; 
    // 2. client use the wrong password

    // get username and size from header
    string username(header.substr(0, pos));
    string size(header.substr(pos+1));

    // EH1
    if (UP_map.find(username) == UP_map.end()) {
        return;
    }

    // lookup password and decrypt the body.
    const char* password = UP_map[username].c_str();
    const char* buf_ciphertext = request->request_body;
    unsigned int size_ciphertext;
    cvt_int(size.c_str(), strlen(size.c_str()), size_ciphertext);

    unsigned int size_cleartext;
    void *decryptedmessage;
    decryptedmessage = fs_decrypt(password, buf_ciphertext, size_ciphertext, &size_cleartext);

    // EH2
    if (decryptedmessage == nullptr) {
        return;
    }

    // judge the type of request 
    unsigned int session, sequence;
    char cr_type;
    string pathname;
    unsigned int block;

    request_type type;

    // Parse the request
    // Get the type of request, invalid if the request is not in the right format 
    // or give wrong info (e.g. non-existed file)
    char* crequest = (char*)decryptedmessage;
    unsigned int l = 0, r = 0;
    // get operation name
    for (r = 0; r < size_cleartext && crequest[r] != ' '; r++); 
    string op_str(crequest+l, r-l);
    if  (op_str.compare("FS_SESSION") == 0)    { 
        type = SESSION; 
        if (size_cleartext-r-1 > 2*MAXSIZE_INT+2) { return; }
    } 
    else if (op_str.compare("FS_CREATE") == 0)     { 
        type = CREATE;  
        if (size_cleartext-r-1 > 2*MAXSIZE_INT+4+FS_MAXPATHNAME+1) { return; }
    } 
    else if (op_str.compare("FS_DELETE") == 0)     { 
        type = DELETE;  
        if (size_cleartext-r-1 > 2*MAXSIZE_INT+3+FS_MAXPATHNAME) { return; }
    } 
    else if (op_str.compare("FS_READBLOCK") == 0)  { 
        type = READ;    
        if (size_cleartext-r-1 > 3*MAXSIZE_INT+4+FS_MAXPATHNAME) { return; }
    } 
    else if (op_str.compare("FS_WRITEBLOCK") == 0) { 
        type = WRITE;   
        if (size_cleartext-r-1 > 3*MAXSIZE_INT+4+FS_MAXPATHNAME+FS_BLOCKSIZE) { return; }
    } 
    else                                           { return;}
    l = r+1;

    // get session number
    for (r = l; r < size_cleartext && crequest[r] != ' '; r++);
    if (!cvt_int(crequest+l, r-l, session)) { return; }
    l = r+1;

    // get seq num
    for (r = l; r < size_cleartext && crequest[r] != ' '&&crequest[r] != '\0'; r++);
    if (!cvt_int(crequest+l, r-l, sequence)) { return; }
    l = r+1;

    // EH3: username -> session and session -> sequence (except for fs_session)
    if (type != SESSION) {
        ssmap_lock.lock();
        if (US_map.find(username) == US_map.end()) {
            ssmap_lock.unlock();
            return;
        }
        auto found_sesh = US_map[username].find(session);
        if (found_sesh == US_map[username].end() || SS_map[session] >= sequence) { 
            ssmap_lock.unlock();
            return;
        }
        SS_map[session] = sequence;
        ssmap_lock.unlock();
    } 
    
    char write_data[FS_BLOCKSIZE];
    bool parse_succ = parse_req(crequest+r, type, size_cleartext-r, pathname, block, cr_type, write_data);
    delete [] (char*)decryptedmessage;
    if (!parse_succ) {
        return;
    }

    // Conduct requests
    if (type == SESSION) {
        // operation of fs_session
        if (session != 0) {
            return;
        }
        ssmap_lock.lock();
        
        if (session_max) {
            // no more sessions avalable
            ssmap_lock.unlock();
            return;
        }
        if (session_id == numeric_limits<unsigned int>::max()) {
            // Num of session reaches to the max
            session_max = true;
        }
        US_map[username].insert(session_id);
        SS_map[session_id] = sequence;
        session = session_id++;

        ssmap_lock.unlock();
    }
    const char* c_username = username.c_str();
    char read_data[FS_BLOCKSIZE];
    bool op_succ = conduct_operation(pathname, c_username, block, cr_type, write_data, read_data, type);
    if (!op_succ) {
       return;
    }
    send_response(type, session, sequence, read_data, socket, password);
}

// Recursively traverse the existed file system
// Load free_blocks and fs_locks
void traverse_fs(unsigned int inode_block) {
    fs_inode inode;
    disk_readblock(inode_block, (void*)(&inode));
   
    num_block_remain--;
    free_blocks.erase(remove(free_blocks.begin(), free_blocks.end(), inode_block)); 
    for (unsigned int i = 0; i < inode.size; i++) {
        num_block_remain--;
        free_blocks.erase(remove(free_blocks.begin(), free_blocks.end(), inode.blocks[i])); 
    }
    mm_fs_locks.add_lock(inode_block);

    if (inode.type == 'f' || inode.size == 0) {
        return;
    }
    
    fs_direntry* blk_direts = new fs_direntry[FS_DIRENTRIES];    
    for (unsigned int i = 0; i < inode.size; i++) {
        disk_readblock(inode.blocks[i], (void*)blk_direts);
        for (unsigned int j = 0; j < FS_DIRENTRIES; j++) {
            if (blk_direts[j].inode_block == 0) continue;
            traverse_fs(blk_direts[j].inode_block);
        }
    }
    delete [] blk_direts;
}

// Thread function for each client request
static void service(int socket) {
    // Get request information 
    string buffer;
    char buf;
    unsigned int message_size;
    request_t request;
    char username[FS_MAXUSERNAME+1];
    buffer = "";
    unsigned int total_bytes = 0;

    // Receive header
    while (true) {
        int byteReceived = recv(socket, &buf, 1, 0);
        if (byteReceived <= 0) {
            perror("recv");
        }
        request.header += buf;
        total_bytes++;
        if (buf == '\0') break;
        if (total_bytes == FS_MAXUSERNAME+2+MAXSIZE_INT) break;
    }
    if (total_bytes==FS_MAXUSERNAME+12 || 
        count_spaces(request.header.c_str())!=1) {
        close(socket);
        return;
    }
    size_t pos = request.header.find(' ');    
    string username_str(request.header.substr(0, pos));
    string size_str(request.header.substr(pos+1));

    if (username_str.size() > FS_MAXUSERNAME ||
        !cvt_int(size_str.c_str(), strlen(size_str.c_str()), message_size)) {
        close(socket);
        return;
    }

    strcpy(username, username_str.c_str());

    // Receive response body
    request.request_body = new char[message_size]; 
    recv(socket, request.request_body, message_size, MSG_WAITALL);
    
    // Deal with the request
    message_handler(&request, socket);

    delete [] request.request_body;
    close(socket);
}

int main (int argc, char** argv) {
    // Initialize: 
    // 1. get server and server_port from arguments
    // 2. read the lists of usernames and passwords from stdin
    // 3. initialze the list of free blocks(empty fs or used fs)
    // 4. set up the socket 
    
    // get server and server_port from arguments
    if (argc > 2) {
        cerr << "error: invalid passed arguments!" << endl;
        exit(1);
    }
    int server_port;
    if (argc == 2) {
        server_port = atoi(argv[1]);
    } else {
        server_port = 0; 
    }
    
    // read the lists of usernames and passwords from stdin
    string line;
    char username[FS_MAXUSERNAME+1]; 
    char password[FS_MAXPASSWORD+1];
    while (cin) {
        getline(cin, line);
        sscanf(line.c_str(), "%s %s", username, password);
        UP_map[username] = password;
    }
    
    // initialze the list of free blocks(empty fs or used fs)
    // initialize the fs_locks
    for (unsigned int i = 0; i < FS_DISKSIZE; i++) {
        free_blocks.push_back(i);
    } 
    num_block_remain = FS_DISKSIZE;
    traverse_fs(0);
    
    // socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        cerr << "socket error" << endl;
        return 1;
    }

    int newConnect;
    struct sockaddr_in addrServer;
    struct sockaddr_in addrClient;

    // bind
    addrServer.sin_family = AF_INET;
    addrServer.sin_port = htons(server_port);
    addrServer.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addrServer, sizeof(addrServer)) == -1) {
        close(sock);
        cerr << "bind error" << endl;
        return 1;
    }
    int rval;
    socklen_t len = sizeof(addrServer);
    rval = getsockname(sock, (struct sockaddr *)&addrServer, &len);
    if (rval != 0) {
        cerr << "get sockname failed" << endl;
        return 1;
    }
    cout << "\n@@@ port " << ntohs(addrServer.sin_port) << endl;
    
    // listen
    if (listen(sock, 10) == -1) {
        close(sock);
        cerr << "listen error" << endl;
        return 1;
    }
    socklen_t addr_size = sizeof(addrClient);
    
    // while loop to accept, send/receive 
    while (true) {
        newConnect = accept(sock, (struct sockaddr *)&addrClient, &addr_size);
        if (newConnect == -1) {
            close(sock);
            cerr << "accept error" << endl;
            continue;
        }
        thread request(service, newConnect);
        request.detach();
    }
}
