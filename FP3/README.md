# LangOS Distributed File System

A distributed file system implementation in C for collaborative document editing, similar to Google Docs. This system supports concurrent access, fine-grained locking, access control, and real-time streaming.

## Architecture

The system consists of three main components:

1. **Name Server (NM)**: Central coordinator managing file metadata, access control, and routing requests
2. **Storage Servers (SS)**: Handle actual file storage, concurrent access, and data persistence
3. **Clients**: User interface for interacting with the file system

## Features

### Core Functionality
- ✅ **File Operations**: Create, read, write, delete files
- ✅ **Access Control**: Owner-based permissions with read/write access management
- ✅ **Concurrent Access**: Multiple users can access files simultaneously
- ✅ **Sentence-Level Locking**: Fine-grained locking for concurrent writes
- ✅ **Undo Support**: Revert the last change made to a file
- ✅ **File Streaming**: Word-by-word content streaming with delays
- ✅ **Command Execution**: Execute file contents as shell commands
- ✅ **Efficient Search**: Trie-based file search with O(m) complexity where m = filename length
- ✅ **Caching**: Recent search results cached for faster access
- ✅ **Data Persistence**: Files and metadata persist across restarts
- ✅ **Comprehensive Logging**: All operations logged with timestamps

### Commands

#### View Files
```bash
VIEW          # List files you have access to
VIEW -a       # List all files in the system
VIEW -l       # List your files with details (word count, size, etc.)
VIEW -al      # List all files with details
```

#### File Operations
```bash
CREATE <filename>                  # Create a new file
READ <filename>                    # Read file contents
DELETE <filename>                  # Delete a file (owner only)
INFO <filename>                    # Get detailed file information
```

#### Writing to Files
```bash
WRITE <filename> <sentence_number>
<word_index> <content>
<word_index> <content>
...
ETIRW                             # Complete the write operation
```

#### Access Management
```bash
ADDACCESS -R <filename> <username>  # Grant read access
ADDACCESS -W <filename> <username>  # Grant write access
REMACCESS <filename> <username>     # Remove access
```

#### Advanced Features
```bash
STREAM <filename>                  # Stream file word-by-word (0.1s delay)
EXEC <filename>                    # Execute file as shell commands
UNDO <filename>                    # Undo last change
LIST                               # List all users
```

## Building the System

### Prerequisites
- GCC compiler
- POSIX threads (pthread)
- Linux/Unix environment

### Compilation
```bash
make all          # Build all components
make name_server  # Build only name server
make storage_server  # Build only storage server
make client       # Build only client
```

### Cleaning
```bash
make clean        # Remove binaries and logs
make cleanall     # Remove everything including storage directories
```

## Running the System

### Step 1: Start the Name Server
```bash
./name_server
```
The Name Server will start on port 8080 by default.

### Step 2: Start Storage Servers
Open new terminals and start one or more storage servers:

```bash
# Storage Server 1
./storage_server 9001 9101 storage1/

# Storage Server 2 (optional)
./storage_server 9002 9102 storage2/
```

Parameters:
- `9001`: Port for Name Server communication
- `9101`: Port for Client communication
- `storage1/`: Directory for storing files

### Step 3: Start Clients
Open new terminals for each client:

```bash
./client
```

When prompted, enter your username. Multiple clients can connect simultaneously.

### Quick Start with Makefile
```bash
# Terminal 1
make run_nm

# Terminal 2
make run_ss1

# Terminal 3
make run_ss2

# Terminal 4
make run_client
```

## Usage Examples

### Example 1: Creating and Writing to a File
```
user1> CREATE notes.txt
File Created Successfully!

user1> WRITE notes.txt 0
Client: 1 Hello world.
Client: ETIRW
Write Successful!

user1> READ notes.txt
Hello world.
```

### Example 2: Collaborative Editing
```
# User 1
user1> WRITE notes.txt 0
Client: 2 This is great!
Client: ETIRW
Write Successful!

# User 2 (simultaneously editing different sentence)
user2> WRITE notes.txt 1
Client: 1 Indeed it is.
Client: ETIRW
Write Successful!

# Result
user1> READ notes.txt
Hello world. This is great! Indeed it is.
```

### Example 3: Access Control
```
user1> CREATE private.txt
File Created Successfully!

user1> ADDACCESS -R private.txt user2
Access granted successfully!

user1> ADDACCESS -W private.txt user3
Access granted successfully!

user1> INFO private.txt
--> File: private.txt
--> Owner: user1
--> Access: user1 (RW), user2 (R), user3 (RW)
...
```

### Example 4: File Execution
```
user1> CREATE script.txt
File Created Successfully!

user1> WRITE script.txt 0
Client: 1 echo Hello from script!
Client: ETIRW
Write Successful!

user1> EXEC script.txt
Hello from script!
```

### Example 5: Streaming
```
user1> STREAM notes.txt
Hello world. This is great! Indeed it is.
```
(Words appear one by one with 0.1-second delays)

## Technical Implementation

### Efficient Search (O(m) complexity)
- **Trie Data Structure**: Files indexed in a trie for O(m) lookup where m = filename length
- **Caching**: Recently accessed file metadata cached for 60 seconds
- **HashMap Alternative**: Can be extended to use hash tables for O(1) average case

### Concurrency and Locking
- **Sentence-Level Locks**: Each file tracks locked sentences by user
- **Pthread Mutexes**: Thread-safe operations across all components
- **Lock Ordering**: Global lock → File lock to prevent deadlocks

### Data Persistence
- **File Storage**: Files stored in designated storage server directories
- **Metadata**: Serialized to `nm_data.dat` for Name Server persistence
- **Crash Recovery**: System recovers file structure on restart

### Networking
- **TCP Sockets**: Reliable communication between all components
- **Message Protocol**: Fixed-size message structure for predictable transmission
- **Multi-threaded Servers**: Each connection handled in separate thread

### Logging System
- **Component Logs**: Separate log files for NM, SS, and clients
- **Timestamps**: All operations logged with precise timestamps
- **Request/Response Tracking**: Full audit trail of all operations

## Error Handling

The system includes comprehensive error handling:

- `ERR_FILE_NOT_FOUND`: Requested file doesn't exist
- `ERR_FILE_EXISTS`: Attempting to create duplicate file
- `ERR_ACCESS_DENIED`: User lacks required permissions
- `ERR_SENTENCE_LOCKED`: Sentence currently locked by another user
- `ERR_INVALID_INDEX`: Word/sentence index out of range
- `ERR_NOT_OWNER`: Operation requires ownership
- `ERR_CONNECTION_FAILED`: Network communication failure

## Limitations and Future Work

### Current Limitations
- Single Name Server (no fault tolerance for NM)
- Simple round-robin SS allocation
- Single undo operation per file
- No folder hierarchy (bonus feature)
- No checkpointing (bonus feature)

### Potential Enhancements
- **Fault Tolerance**: Multiple Name Servers with leader election
- **Replication**: Automatic file replication across storage servers
- **Load Balancing**: Intelligent SS selection based on load
- **Compression**: File compression for large documents
- **Encryption**: End-to-end encryption for sensitive data

## File Structure

```
FP3/
├── common.h              # Shared structures and constants
├── common.c              # Utility functions
├── name_server.c         # Name Server implementation
├── storage_server.c      # Storage Server implementation
├── client.c              # Client implementation
├── Makefile              # Build configuration
├── README.md             # This file
├── nm_data.dat           # Name Server persistent data (generated)
├── NM.log                # Name Server logs (generated)
├── SS.log                # Storage Server logs (generated)
├── CLIENT.log            # Client logs (generated)
└── storage*/             # Storage directories (generated)
```

## Testing

### Basic Functionality Test
```bash
# Start system (NM + 2 SS + 1 Client)

# Test file operations
CREATE test.txt
WRITE test.txt 0
1 Hello world.
ETIRW
READ test.txt
INFO test.txt
DELETE test.txt
```

### Concurrent Access Test
```bash
# Start system with 2 clients

# Client 1
CREATE shared.txt
ADDACCESS -W shared.txt user2

# Client 2
WRITE shared.txt 0
1 From user2.
ETIRW

# Client 1 (verify)
READ shared.txt
```

### Sentence Locking Test
```bash
# Client 1
WRITE shared.txt 0
# (Don't send ETIRW yet)

# Client 2 (should fail)
WRITE shared.txt 0
# ERROR: Sentence is locked by another user

# Client 1
ETIRW
```

## Troubleshooting

### "Failed to connect to Name Server"
- Ensure Name Server is running
- Check if port 8080 is available
- Verify firewall settings

### "No storage servers available"
- Start at least one storage server before creating files
- Check storage server logs for errors

### "Sentence is locked by another user"
- Wait for other user to complete WRITE operation
- Ensure previous WRITE was terminated with ETIRW

### Compilation Errors
- Ensure GCC and pthread library are installed
- Use `-pthread` flag for compilation
- Check for missing header files

## Performance Characteristics

- **File Lookup**: O(m) where m = filename length (Trie-based)
- **User Lookup**: O(n) where n = number of clients (can be improved with hashmap)
- **Concurrent Reads**: Unlimited, no locking
- **Concurrent Writes**: Per-sentence granularity
- **Network Latency**: Minimal with direct client-SS communication

## Security Considerations

- **Authentication**: Username-based (basic implementation)
- **Access Control**: Owner-based permission model
- **Command Execution**: Runs on Name Server (security risk in production)
- **Input Validation**: Basic validation implemented

⚠️ **Note**: This is an educational implementation. Production systems require enhanced security measures.

## Credits

**LangOS Distributed File System**  
Implementation for OSN Course Project  
Distributed Systems and Network File System Design

## License

Educational use only - Course Project Implementation
