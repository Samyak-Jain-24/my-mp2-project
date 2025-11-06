# LangOS DFS - Quick Reference Guide

## Quick Start (3 Steps)

### 1. Build Everything
```bash
make all
```

### 2. Start Components (4 terminals)

**Terminal 1: Name Server**
```bash
./name_server
```

**Terminal 2: Storage Server 1**
```bash
./storage_server 9001 9101 storage1/
```

**Terminal 3: Storage Server 2** *(optional)*
```bash
./storage_server 9002 9102 storage2/
```

**Terminal 4: Client**
```bash
./client
# Enter username when prompted
```

### 3. Try Commands
```
VIEW
CREATE myfile.txt
WRITE myfile.txt 0
1 Hello world.
ETIRW
READ myfile.txt
```

## Command Cheat Sheet

| Command | Syntax | Example |
|---------|--------|---------|
| **View files** | `VIEW [-a] [-l]` | `VIEW -al` |
| **Create file** | `CREATE <file>` | `CREATE test.txt` |
| **Read file** | `READ <file>` | `READ test.txt` |
| **Write file** | `WRITE <file> <sent#>` | `WRITE test.txt 0` |
| | `<word#> <content>` | `1 Hello` |
| | `ETIRW` | `ETIRW` |
| **Delete file** | `DELETE <file>` | `DELETE test.txt` |
| **File info** | `INFO <file>` | `INFO test.txt` |
| **Stream file** | `STREAM <file>` | `STREAM test.txt` |
| **List users** | `LIST` | `LIST` |
| **Grant access** | `ADDACCESS -R/-W <file> <user>` | `ADDACCESS -W test.txt bob` |
| **Remove access** | `REMACCESS <file> <user>` | `REMACCESS test.txt bob` |
| **Execute file** | `EXEC <file>` | `EXEC script.txt` |
| **Undo change** | `UNDO <file>` | `UNDO test.txt` |

## Common Scenarios

### Scenario 1: Create and Edit a File
```
CREATE notes.txt
WRITE notes.txt 0
1 My first note.
ETIRW
READ notes.txt
```

### Scenario 2: Share a File
```
# As owner (user1)
CREATE shared.txt
ADDACCESS -W shared.txt user2

# As user2
WRITE shared.txt 0
1 Hello from user2!
ETIRW
```

### Scenario 3: Collaborative Editing
```
# User1: Edit sentence 0
WRITE shared.txt 0
1 User1 was here.
ETIRW

# User2: Edit sentence 1 (concurrent!)
WRITE shared.txt 1
1 User2 too!
ETIRW

# Result
READ shared.txt
--> User1 was here. User2 too!
```

### Scenario 4: Execute Commands
```
CREATE backup.txt
WRITE backup.txt 0
1 ls -la
2 echo Done!
ETIRW

EXEC backup.txt
# Executes on Name Server, shows output
```

### Scenario 5: Undo Mistakes
```
WRITE test.txt 0
1 Oops I made a mistake!
ETIRW

UNDO test.txt
# Reverts to previous version
```

## WRITE Command Details

### Basic Word Insertion
```
Current: "Hello world."
Command: WRITE file.txt 0
         2 beautiful
         ETIRW
Result:  "Hello beautiful world."
```

### Multiple Insertions
```
Current: "Hello world."
WRITE file.txt 0
2 big
3 beautiful
ETIRW
Result: "Hello big beautiful world."
```

### Creating New Sentences
```
Current: "Hello"
WRITE file.txt 0
2 world. How
4 you?
ETIRW
Result: "Hello world." "How you?"
```

### Indices Explained
- **Sentence 0**: First sentence
- **Word 1**: Before first word (insert at start)
- **Word N+1**: After last word (append)

## Error Messages

| Error | Meaning | Solution |
|-------|---------|----------|
| File not found | File doesn't exist | Check filename with VIEW |
| File already exists | CREATE on existing file | Use WRITE instead |
| Access denied | No permission | Ask owner for access |
| Sentence locked | Someone editing | Wait for ETIRW |
| Invalid index | Word/sentence out of range | Check with READ first |
| Not the owner | Only owner can do this | Must be file creator |
| User not found | Username doesn't exist | Check with LIST |
| No undo history | Nothing to undo | Only one undo available |

## Tips & Tricks

### Performance
- Cache hits recent files (60s TTL)
- Direct client-SS communication = faster
- Multiple storage servers = load balancing

### Concurrency
- Multiple users can READ simultaneously
- Different sentences can be WRITE simultaneously
- Same sentence blocks until ETIRW

### File Organization
```
VIEW -l          # See all your files with stats
VIEW -al         # See everyone's files
INFO file.txt    # Detailed file information
```

### Access Control
```
# Grant read-only
ADDACCESS -R notes.txt alice

# Grant full access
ADDACCESS -W notes.txt bob

# Remove all access
REMACCESS notes.txt alice
```

### Debugging
- Check logs: `NM.log`, `SS.log`, `CLIENT.log`
- View timestamps, errors, requests
- Each component logs independently

## Keyboard Shortcuts

While using client:
- `Ctrl+C`: Interrupt current operation
- `Ctrl+D`: Same as EXIT
- Type `EXIT`: Graceful disconnect

## File Locations

```
./name_server          # Name Server binary
./storage_server       # Storage Server binary  
./client              # Client binary
./storage1/           # SS1 file storage
./storage2/           # SS2 file storage
./NM.log              # Name Server logs
./SS.log              # Storage Server logs
./CLIENT.log          # Client logs
./nm_data.dat         # NM persistent data
```

## Troubleshooting

### Can't connect to Name Server
```bash
# Check if running
ps aux | grep name_server

# Check port
netstat -tulpn | grep 8080

# Restart
killall name_server
./name_server
```

### Storage Server not responding
```bash
# Check if running
ps aux | grep storage_server

# Check logs
tail -f SS.log

# Restart
killall storage_server
./storage_server 9001 9101 storage1/
```

### "Sentence is locked" error
- Another user is editing
- Wait for them to send ETIRW
- Or ask them to finish
- Timeout: None (manual coordination needed)

### Lost connection during STREAM
- Storage Server may have crashed
- Check SS logs
- Restart Storage Server
- Data should persist

## Advanced Usage

### Multiple Storage Servers
```bash
# Terminal 1
./storage_server 9001 9101 storage1/

# Terminal 2  
./storage_server 9002 9102 storage2/

# Terminal 3
./storage_server 9003 9103 storage3/

# Files distributed via round-robin
```

### Bulk Operations
```bash
# Create test script
cat > commands.txt << EOF
CREATE file1.txt
CREATE file2.txt
CREATE file3.txt
EOF

# Execute via client (manual for now)
```

### Monitoring
```bash
# Watch Name Server logs
tail -f NM.log

# Count files in system
grep "File created" NM.log | wc -l

# Monitor active connections
netstat -an | grep 8080
```

## System Limits

| Resource | Limit | Note |
|----------|-------|------|
| Max Files | 10,000 | Per system |
| Max Clients | 100 | Concurrent |
| Max Storage Servers | 50 | Concurrent |
| Max Filename | 256 chars | Including extension |
| Max File Size | 8 KB | MAX_CONTENT |
| Max Users per File | 50 | Access list |
| Cache Size | 100 entries | 60s TTL |
| Undo Depth | 1 level | Last change only |

## Performance Benchmarks

| Operation | Time | Notes |
|-----------|------|-------|
| File search | O(m) | m = filename length |
| File create | ~5ms | Including persistence |
| File read | ~2ms | Direct SS connection |
| File write | ~10ms | Lock + update |
| Cache hit | <1ms | 50x faster |
| Stream word | 100ms | Fixed delay |

## Best Practices

### Do's ✓
- Use descriptive filenames
- Grant minimal necessary access
- Send ETIRW promptly after WRITE
- Check VIEW before CREATE
- Use INFO to verify access
- Monitor logs for debugging

### Don'ts ✗
- Don't leave WRITE sessions open
- Don't share passwords (use access control)
- Don't use special characters in filenames
- Don't rely on multiple undo levels
- Don't assume file exists (check first)

## Getting Help

### Documentation
- `README.md` - Overview and setup
- `IMPLEMENTATION.md` - Technical details
- `QUICK_REFERENCE.md` - This file

### Community
- Check logs first
- Review error messages
- Test with simple cases
- Verify all components running

### Development
```bash
# Rebuild after changes
make clean
make all

# Reset everything
make cleanall
make all
```

---

**Remember**: Name Server must start first, then Storage Servers, then Clients!

**Need more help?** Check `IMPLEMENTATION.md` for detailed technical information.
