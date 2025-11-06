## Testing the WRITE fix

The segmentation fault was caused by:
1. Using `strtok` directly on strings that were being reused
2. `strtok` modifies the original string
3. Not making copies before tokenization

### Fix Applied:
1. Created copies of `msg->data`, `sentences[i]`, and `word_content` before using `strtok`
2. Added better bounds checking to prevent buffer overflows
3. Added null checks and length validation
4. Improved the `parse_sentences` function with better error handling

### How to Test:

**Terminal 1: Start Name Server**
```bash
./name_server
```

**Terminal 2: Start Storage Server**
```bash
mkdir -p storage1
./storage_server 9001 9101 storage1/
```

**Terminal 3: Client Test**
```bash
./client
# Enter username: testuser

# Test 1: Basic write
CREATE test.txt
WRITE test.txt 0
1 Hello world.
ETIRW
READ test.txt

# Test 2: Multiple words
WRITE test.txt 0
2 beautiful
ETIRW
READ test.txt

# Test 3: New sentence
WRITE test.txt 0
3 How are you?
ETIRW
READ test.txt
```

### Expected Behavior:
- Storage server should NOT crash with "Segmentation fault"
- All WRITE operations should complete successfully
- Files should be readable and show correct content

### Root Cause:
The issue was in `storage_server.c` line ~384-450 where:
- `strtok(msg->data, "\n")` was modifying the message data
- `strtok(sentences[i], " ")` was destroying the sentence array
- Nested `strtok` calls were interfering with each other

### Solution:
Always create a copy before using `strtok`:
```c
char data_copy[MAX_CONTENT];
strncpy(data_copy, msg->data, MAX_CONTENT - 1);
char* line = strtok(data_copy, "\n");  // Safe!
```
