# LangOS DFS - Implementation Details

## Table of Contents
1. [Architecture Overview](#architecture-overview)
2. [Core Components](#core-components)
3. [Communication Protocol](#communication-protocol)
4. [Concurrency Model](#concurrency-model)
5. [Data Structures](#data-structures)
6. [File Operations](#file-operations)
7. [Error Handling](#error-handling)
8. [Performance Optimizations](#performance-optimizations)

## Architecture Overview

### System Design
```
┌─────────────┐          ┌─────────────┐          ┌─────────────┐
│   Client 1  │          │   Client 2  │          │   Client N  │
└──────┬──────┘          └──────┬──────┘          └──────┬──────┘
       │                        │                        │
       └────────────────────────┼────────────────────────┘
                                │
                         ┌──────▼──────┐
                         │ Name Server │ (Port 8080)
                         │   (NM)      │
                         └──────┬──────┘
                                │
                ┌───────────────┼───────────────┐
                │               │               │
         ┌──────▼──────┐ ┌─────▼──────┐ ┌─────▼──────┐
         │ Storage     │ │ Storage    │ │ Storage    │
         │ Server 1    │ │ Server 2   │ │ Server N   │
         └─────────────┘ └────────────┘ └────────────┘
```

### Communication Flows

#### 1. File Read Flow
```
Client → NM: "I want to read file.txt"
NM → Client: "Connect to SS1 at 127.0.0.1:9101"
Client → SS1: "Send me file.txt"
SS1 → Client: [file contents]
```

#### 2. File Write Flow
```
Client → NM: "I want to write to file.txt"
NM → Client: "Connect to SS1 at 127.0.0.1:9101"
Client → SS1: "Lock sentence 2, write data"
SS1: [Applies lock, processes writes]
Client → SS1: "ETIRW (release lock)"
SS1 → Client: "Write successful"
```

#### 3. File Create Flow
```
Client → NM: "Create file.txt"
NM: [Selects SS using round-robin]
NM → SS1: "Create file.txt"
SS1: [Creates file, metadata]
SS1 → NM: "Success"
NM: [Updates metadata, trie]
NM → Client: "File created"
```

## Core Components

### 1. Name Server (name_server.c)

#### Responsibilities
- Client and Storage Server registration
- File metadata management
- Access control enforcement
- Request routing
- Persistent storage of metadata
- Efficient file search
- Caching

#### Key Data Structures
```c
// File metadata stored in trie
typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    int ss_id;
    AccessEntry access_list[MAX_ACCESS_LIST];
    time_t created_time, modified_time, accessed_time;
    // ... more fields
} FileMetadata;

// Trie for O(m) lookup
typedef struct TrieNode {
    struct TrieNode* children[256];
    int is_end;
    FileMetadata* file_info;
} TrieNode;

// Storage server info
typedef struct {
    int ss_id;
    char ip[INET_ADDRSTRLEN];
    int nm_port, client_port;
    int active;
    char files[MAX_FILES][MAX_FILENAME];
} StorageServerInfo;
```

#### Thread Model
- Main thread: Accepts connections
- Per-connection threads: Handle client/SS requests
- Mutex protection: Global state, file metadata

### 2. Storage Server (storage_server.c)

#### Responsibilities
- Physical file storage
- Concurrent read/write handling
- Sentence-level locking
- Undo functionality
- File streaming
- Data persistence

#### Key Data Structures
```c
// Lock information per file
typedef struct {
    char filename[MAX_FILENAME];
    int locked_sentence;
    char locked_by[MAX_USERNAME];
    char undo_content[MAX_CONTENT];
    int has_undo;
} FileLockInfo;

// Per-file mutex array
pthread_mutex_t file_locks[MAX_FILES];
```

#### Thread Model
- Main thread: Initialization
- NM listener thread: Handles NM requests
- Client listener thread: Handles client requests
- Per-request threads: Process individual operations

### 3. Client (client.c)

#### Responsibilities
- User interface
- Command parsing
- Name Server communication
- Direct Storage Server communication
- Result presentation

#### Command Processing
```c
// Command flow
User Input → Parse → Create Message → Send to NM
         ↓
    Get SS Info → Connect to SS → Direct Communication
         ↓
    Display Result
```

## Communication Protocol

### Message Structure
```c
typedef struct {
    int op_code;                    // Operation type
    char username[MAX_USERNAME];    // User identifier
    char filename[MAX_FILENAME];    // Target file
    char data[MAX_CONTENT];         // Payload
    int sentence_number;            // For WRITE operations
    int word_index;                 // For WRITE operations
    int flags;                      // Command flags (-a, -l, etc.)
    int error_code;                 // Response status
    char error_msg[256];            // Error details
    int data_size;                  // Payload size
} Message;
```

### Operation Codes
```c
#define OP_VIEW 1           // List files
#define OP_READ 2           // Read file
#define OP_CREATE 3         // Create file
#define OP_WRITE 4          // Write to file
#define OP_DELETE 5         // Delete file
#define OP_INFO 6           // Get file info
#define OP_STREAM 7         // Stream file
#define OP_LIST 8           // List users
#define OP_ADDACCESS 9      // Grant access
#define OP_REMACCESS 10     // Revoke access
#define OP_EXEC 11          // Execute file
#define OP_UNDO 12          // Undo changes
#define OP_REGISTER_SS 20   // SS registration
#define OP_REGISTER_CLIENT 21 // Client registration
```

## Concurrency Model

### Locking Strategy

#### Name Server
```c
// Global lock for metadata
pthread_mutex_t nm_lock = PTHREAD_MUTEX_INITIALIZER;

// Protected operations:
- File creation/deletion
- Access control changes
- SS/Client registration
- Metadata updates
```

#### Storage Server
```c
// Per-file locks
pthread_mutex_t file_locks[MAX_FILES];

// Two-level locking:
1. File-level lock: Protects file access
2. Sentence-level lock: Application-level coordination

// Lock acquisition order:
global_lock → file_locks[i]  (prevents deadlock)
```

### Concurrent Access Rules

#### Reads
- **Multiple simultaneous reads**: Allowed
- **Read during write**: Allowed (sees old or new data)
- **No locking required**: Read-only operations don't acquire locks

#### Writes
- **Sentence-level exclusion**: Only one writer per sentence
- **Lock held from WRITE to ETIRW**: Ensures atomic multi-word updates
- **Different sentences**: Can be written concurrently

```c
// Example: Two users can write simultaneously
User1: WRITE file.txt 0  (sentence 0)
User2: WRITE file.txt 1  (sentence 1)  ✓ Allowed

User1: WRITE file.txt 0  (sentence 0)
User2: WRITE file.txt 0  (sentence 0)  ✗ Blocked until User1 sends ETIRW
```

## Data Structures

### 1. Trie (Efficient File Search)

#### Structure
```
Root
├── 'f'
│   └── 'i'
│       └── 'l'
│           └── 'e'
│               └── '.'
│                   └── 't'
│                       └── 'x'
│                           └── 't' [END: FileMetadata*]
└── 't'
    └── 'e'
        └── 's'
            └── 't'
                └── '.' 
                    └── 't'
                        └── 'x'
                            └── 't' [END: FileMetadata*]
```

#### Complexity
- **Insert**: O(m) where m = filename length
- **Search**: O(m)
- **Delete**: O(m)
- **Space**: O(ALPHABET_SIZE × N × AVG_LENGTH)

#### Implementation
```c
TrieNode* create_trie_node() {
    TrieNode* node = calloc(1, sizeof(TrieNode));
    // 256 children for full ASCII range
    for (int i = 0; i < 256; i++) {
        node->children[i] = NULL;
    }
    return node;
}

void trie_insert(TrieNode* root, const char* filename, FileMetadata* file) {
    TrieNode* curr = root;
    for (int i = 0; filename[i]; i++) {
        unsigned char idx = (unsigned char)filename[i];
        if (!curr->children[idx]) {
            curr->children[idx] = create_trie_node();
        }
        curr = curr->children[idx];
    }
    curr->is_end = 1;
    curr->file_info = file;
}
```

### 2. Cache (Recent Searches)

#### Structure
```c
typedef struct {
    char filename[MAX_FILENAME];
    FileMetadata* file_info;
    time_t timestamp;
} CacheEntry;

CacheEntry search_cache[100];  // Fixed-size cache
```

#### Cache Policy
- **Eviction**: Replace oldest entry when full
- **TTL**: 60 seconds
- **Hit rate**: High for repeated operations

#### Usage
```c
FileMetadata* search_file_cached(const char* filename) {
    // 1. Check cache
    for (int i = 0; i < cache_size; i++) {
        if (strcmp(cache[i].filename, filename) == 0) {
            if (time(NULL) - cache[i].timestamp < 60) {
                return cache[i].file_info;  // Cache hit
            }
        }
    }
    
    // 2. Search trie
    FileMetadata* file = trie_search(file_trie_root, filename);
    
    // 3. Update cache
    if (file) update_cache(filename, file);
    
    return file;
}
```

## File Operations

### 1. WRITE Operation (Complex Case)

#### Sentence Parsing
```c
// Input: "Hello world. How are you? I'm fine."
// Output: 3 sentences
sentences[0] = "Hello world."
sentences[1] = "How are you?"
sentences[2] = "I'm fine."

// Sentence delimiters: '.', '!', '?'
// Even in middle of words: "e.g." creates 2 sentences
```

#### Word Insertion
```c
// Current sentence 0: "Hello world."
// Command: 2 beautiful
// Result: "Hello beautiful world."

// Implementation:
1. Parse sentence into words: ["Hello", "world."]
2. Insert at index 2: Shift words right
3. New words: ["Hello", "beautiful", "world."]
4. Reconstruct: "Hello beautiful world."
```

#### Sentence Splitting
```c
// Current: "Hello world"
// Command: 2 there! How
// Result: Two sentences!
// "Hello there!" 
// "How world"

// Detection: Word ends with delimiter
words[i][len-1] == '.' || '!' || '?'

// Process:
1. Parse words: ["Hello", "there!", "How", "world"]
2. Detect delimiter in "there!"
3. Split into sentences
4. Update sentence array
```

### 2. UNDO Operation

#### Implementation
```c
// Before write
strncpy(lock_info->undo_content, old_content, MAX_CONTENT);
lock_info->has_undo = 1;

// On UNDO
if (lock_info->has_undo) {
    save_file_content(filename, lock_info->undo_content);
    lock_info->has_undo = 0;
}
```

#### Limitations
- Only one undo level
- Undo is file-wide (reverts entire file)
- Any user can undo any change

### 3. STREAM Operation

#### Implementation
```c
void handle_stream_file(int client_sock, Message* msg) {
    char* content = load_file_content(msg->filename);
    
    // Send success
    msg->error_code = ERR_SUCCESS;
    send_message(client_sock, msg);
    
    // Stream word by word
    char* word = strtok(content, " \n\t");
    while (word != NULL) {
        strcpy(msg->data, word);
        send_message(client_sock, msg);
        usleep(100000);  // 0.1 second delay
        word = strtok(NULL, " \n\t");
    }
    
    // Send stop signal
    strcpy(msg->data, "STOP");
    send_message(client_sock, msg);
}
```

### 4. EXEC Operation

#### Implementation
```c
// NM reads file from SS
char* content = get_file_from_ss(filename);

// Parse into commands
char* command = strtok(content, "\n");

// Execute each command
while (command != NULL) {
    FILE* pipe = popen(command, "r");
    if (pipe) {
        // Read output
        while (fgets(line, sizeof(line), pipe)) {
            strcat(output, line);
        }
        pclose(pipe);
    }
    command = strtok(NULL, "\n");
}

// Send output to client
strcpy(msg->data, output);
send_message(client_sock, msg);
```

## Error Handling

### Error Code System
```c
#define ERR_SUCCESS 0
#define ERR_FILE_NOT_FOUND 1
#define ERR_FILE_EXISTS 2
#define ERR_ACCESS_DENIED 3
#define ERR_SENTENCE_LOCKED 4
#define ERR_INVALID_INDEX 5
#define ERR_SERVER_ERROR 6
#define ERR_CONNECTION_FAILED 7
#define ERR_INVALID_COMMAND 8
#define ERR_NOT_OWNER 9
#define ERR_USER_NOT_FOUND 10
#define ERR_SS_NOT_FOUND 11
#define ERR_NO_UNDO 12
```

### Error Propagation
```
Storage Server → Name Server → Client
      ↓               ↓            ↓
  Sets error     Forwards     Displays
   in msg         error        to user
```

### Example Error Flow
```c
// Client requests write without permission
Client → NM: WRITE protected.txt

// NM checks access
if (!check_access(file, username, ACCESS_WRITE)) {
    msg->error_code = ERR_ACCESS_DENIED;
    strcpy(msg->error_msg, "Access denied");
    send_message(client_sock, msg);
    return;
}

// Client receives error
if (msg.error_code != ERR_SUCCESS) {
    print_error(msg.error_code, "WRITE");
    // Output: "ERROR [WRITE]: Access denied"
}
```

## Performance Optimizations

### 1. Trie vs Linear Search
```
Operation         | Linear | Trie
------------------|--------|-------
Search 1 file     | O(n)   | O(m)
Search 100 files  | O(100n)| O(100m)
Insert            | O(1)   | O(m)
Delete            | O(n)   | O(m)

Where: n = total files, m = filename length
```

### 2. Caching Benefits
```
Without cache: Every INFO requires trie traversal
With cache:    Repeated INFO is instant

Example: 
- First INFO file.txt: 0.5ms (trie search)
- Second INFO file.txt: 0.01ms (cache hit)
- Speedup: 50x
```

### 3. Direct Client-SS Communication
```
Via NM:          Client → NM → SS → NM → Client
Direct:          Client → SS → Client

Latency reduced by 2 hops (~50% faster)
```

### 4. Round-Robin SS Selection
```c
int ss_index = file_count % ss_count;

// Ensures even distribution
// O(1) complexity
// No complex load balancing needed
```

## Logging System

### Log Format
```
[TIMESTAMP] [LEVEL] MESSAGE

Example:
[2025-11-05 14:32:15] [INFO] File created: test.txt by user1
[2025-11-05 14:32:20] [REQUEST] From 127.0.0.1:9001 [user1] - READ
[2025-11-05 14:32:20] [RESPONSE] To 127.0.0.1:9001 - Status: 0, Success
```

### Log Levels
- **INFO**: General information
- **REQUEST**: Incoming requests
- **RESPONSE**: Outgoing responses
- **ERROR**: Error conditions

### Log Files
- `NM.log`: Name Server operations
- `SS.log`: Storage Server operations
- `CLIENT.log`: Client operations

## Persistence

### Name Server Data
```c
// Saved to nm_data.dat
- int file_count
- FileMetadata files[file_count]
- int ss_count
- StorageServerInfo storage_servers[ss_count]

// Binary format for efficiency
fwrite(&file_count, sizeof(int), 1, fp);
fwrite(files, sizeof(FileMetadata), file_count, fp);
```

### Storage Server Data
```
storage1/
├── file1.txt         # Actual file content
├── file1.txt.meta    # Metadata (timestamps, etc.)
├── file2.txt
└── file2.txt.meta
```

### Recovery Process
```
1. NM starts → load_persistent_data()
2. Read nm_data.dat
3. Rebuild trie from loaded files
4. SS starts → scan storage directory
5. Register with NM
6. System ready
```

## Access Control

### Permission Model
```
Owner: Always has RW
Users: Can have R or RW (set by owner)

access_list[i] = {
    username: "user2",
    access_type: ACCESS_READ (1) or ACCESS_WRITE (2)
}
```

### Check Algorithm
```c
int check_access(FileMetadata* file, const char* username, int required) {
    // Owner check
    if (strcmp(file->owner, username) == 0) return 1;
    
    // Access list check
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i].username, username) == 0) {
            if (required == ACCESS_READ) {
                return file->access_list[i].access_type >= ACCESS_READ;
            } else if (required == ACCESS_WRITE) {
                return file->access_list[i].access_type >= ACCESS_WRITE;
            }
        }
    }
    
    return 0;  // No access
}
```

## Implementation Highlights

### Key Design Decisions

1. **Trie for file search**: O(m) complexity requirement satisfied
2. **Fixed-size messages**: Simplifies network code
3. **Sentence-level locking**: Balance between granularity and complexity
4. **Direct client-SS communication**: Reduces NM load
5. **Pthread mutexes**: Standard, reliable concurrency
6. **Binary persistence**: Fast load/save
7. **Round-robin SS allocation**: Simple and effective

### Code Statistics
- **Total Lines**: ~2500 lines of C code
- **Components**: 4 files (common, NM, SS, client)
- **Functions**: ~60 functions
- **Thread Safety**: All shared state protected
- **Error Handling**: Comprehensive coverage

### Testing Coverage
- ✓ Single client operations
- ✓ Multi-client concurrent access
- ✓ Sentence-level locking
- ✓ Access control enforcement
- ✓ Persistence across restarts
- ✓ Error conditions
- ✓ Network failures
- ✓ Large files

---

This implementation provides a solid foundation for a distributed file system with room for extensions like replication, checkpointing, and folder hierarchies.
