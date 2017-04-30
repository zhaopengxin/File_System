CFLAGS = -std=c++11 -g -pthread
LFLAGS = -ldl 

SERVER_LIB = libfs_server.o 
CLIENT_LIB = libfs_client.o

CPPS = test*.cpp

server: fs.cc
	g++ fs.cc $(SERVER_LIB) -o server $(CFLAGS) $(LFLAGS)

tests: client_spec test_error_username test_concurrent test_concurrent4 test_concurrent2 test_delete test_delete2 test_session test_create test_rwblock test_seqnum test_invalid test_basics test_send_any test_invalidname test_multiusersession test_manyrw test_mixup test_everything test_basic test_rw test_delete3 test_concurrent3 test_header test_err
	
client_spec: $(CPPS)
	g++ test_spec.cpp $(CLIENT_LIB) -o client_spec $(CFLAGS)

test_error_username: test_zpx_1_error_username.cpp
	g++ test_zpx_1_error_username.cpp $(CLIENT_LIB) -o client_error_username $(CFLAGS)

test_concurrent: test_zpx_2_concurrent.cpp
	g++ test_zpx_2_concurrent.cpp $(CLIENT_LIB) -o client_concurrent $(CFLAGS)

test_concurrent2: test_wsy_5_fs_concurrent.cpp
	g++ test_wsy_5_fs_concurrent.cpp $(CLIENT_LIB) -o client_concurrent2 $(CFLAGS)

test_concurrent4: test_concurrent.cpp
	g++ test_concurrent.cpp $(CLIENT_LIB) -o client_concurrent4 $(CFLAGS)

test_delete: test_zpx_3_delete.cpp
	g++ test_zpx_3_delete.cpp $(CLIENT_LIB) -o client_delete $(CFLAGS)

test_delete2: test_wsy_4_fs_delete.cpp
	g++ test_wsy_4_fs_delete.cpp $(CLIENT_LIB) -o client_delete2 $(CFLAGS)

test_delete3: test_zpx_5_delete.cpp
	g++ test_zpx_5_delete.cpp $(CLIENT_LIB) -o client_delete3 $(CFLAGS)

test_session: test_wsy_1_fs_session.cpp
	g++ test_wsy_1_fs_session.cpp $(CLIENT_LIB) -o client_session $(CFLAGS)

test_create: test_wsy_2_fs_create.cpp
	g++ test_wsy_2_fs_create.cpp $(CLIENT_LIB) -o client_create $(CFLAGS)

test_rwblock: test_wsy_3_fs_readwriteblock.cpp
	g++ test_wsy_3_fs_readwriteblock.cpp $(CLIENT_LIB) -o client_rwblock $(CFLAGS)

test_seqnum: test_seqnum.cpp
	g++ test_seqnum.cpp $(CLIENT_LIB) -o client_seqnum $(CFLAGS)

test_invalid: test_sby_1_invalid.cpp
	g++ test_sby_1_invalid.cpp $(CLIENT_LIB) -o client_invalid $(CFLAGS)

test_basics: test_sby_2_basics.cpp
	g++ test_sby_2_basics.cpp $(CLIENT_LIB) -o client_basics $(CFLAGS)

test_send_any: test_sby_3_send_anything.cpp
	g++ test_sby_3_send_anything.cpp $(CLIENT_LIB) -o client_sendany $(CFLAGS)

test_mixup: test_sby_4_mixup.cpp
	g++ $^ $(CLIENT_LIB) -o client_mixup $(CFLAGS)

test_header: test_header.cpp
	g++ $^ $(CLIENT_LIB) -o client_header $(CFLAGS)

test_everything: test_sby_5_everything.cpp
	g++ $^ $(CLIENT_LIB) -o client_everything $(CFLAGS)

test_concurrent3: test_concurrent3.cpp
	g++ $^ $(CLIENT_LIB) -o client_concurrent3 $(CFLAGS)

test_invalidname: test_wsy_6_invalidname.cpp
	g++ test_wsy_6_invalidname.cpp $(CLIENT_LIB) -o client_invalidname $(CFLAGS)

test_multiusersession: test_wsy_7_multiusersession.cpp
	g++ test_wsy_7_multiusersession.cpp $(CLIENT_LIB) -o client_multiusersession $(CFLAGS)

test_manyrw: test_wsy_8_manyrw.cpp
	g++ test_wsy_8_manyrw.cpp $(CLIENT_LIB) -o client_manyrw $(CFLAGS)

test_basic: test_wsy_9_basic.cpp
	g++ $^ $(CLIENT_LIB) -o client_basic $(CFLAGS)

test_rw: test_zpx_4_rw.cpp
	g++ $^ $(CLIENT_LIB) -o client_rw $(CFLAGS)

test_err: test_zpx_6_error_handling.cpp
	g++ $^ $(CLIENT_LIB) -o client_err $(CFLAGS)

test_thread: test_ly_thread_1.cpp
	g++ $^ $(CLIENT_LIB) -o client_thread $(CFLAGS)

run_server:
	export FS_CRYPT=CLEAR
	./server 8000 < passwords

run_tests:
	export FS_CRYPT=CLEAR
	./client_spec localhost 8000
	./client_error_username localhost 8000
	./client_concurrent localhost 8000
	./client_concurrent2 localhost 8000
	./client_delete localhost 8000
	./client_delete2 localhost 8000
	./client_session localhost 8000
	./client_create localhost 8000
	./client_rwblock localhost 8000
	./client_seqnum localhost 8000

clean: 
	rm -f server client*
