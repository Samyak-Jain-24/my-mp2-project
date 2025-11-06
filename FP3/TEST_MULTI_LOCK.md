# Test Multiple Sentence Locks

## Setup
1. Start Name Server: `./name_server`
2. Start Storage Server: `./storage_server`
3. Start Client 1: `./client` (login as user1)
4. Start Client 2: `./client` (login as user2)

## Test Scenario

### In Client 1 (user1):
```
CREATE kehde.txt
WRITE kehde.txt 0
1 This is sentence zero.
```
(Don't type ETIRW yet!)

### In Client 2 (user2):
```
WRITE kehde.txt 1
1 This is sentence one.
```
(Don't type ETIRW yet!)

### Expected Result:
- Both clients should successfully lock their respective sentences
- Client 1 has sentence 0 locked
- Client 2 has sentence 1 locked
- No conflict because they're different sentences

### Now in Client 1:
```
ETIRW
```

### Expected Result:
- Client 1's write should complete successfully
- Sentence 0 should be unlocked
- Sentence 1 should still be locked by Client 2

### Now in Client 2:
```
ETIRW
```

### Expected Result:
- Client 2's write should complete successfully
- Sentence 1 should be unlocked
- Both sentences should be updated in the file

### Verify with:
```
READ kehde.txt
```

Should show both sentences updated correctly.

## What Was Fixed

Previously, the file had only ONE lock (locked_sentence and locked_by), so:
- User1 locks sentence 0 → `locked_sentence = 0`
- User2 locks sentence 1 → `locked_sentence = 1` (overwrites!)
- User1 unlocks → `locked_sentence = -1` (clears User2's lock!)
- User2 tries to write → ERROR because lock is gone

Now, the file has an ARRAY of locks (sentence_locks[]), so:
- User1 locks sentence 0 → `locks[0] = {sentence: 0, by: "user1"}`
- User2 locks sentence 1 → `locks[1] = {sentence: 1, by: "user2"}`
- User1 unlocks → removes locks[0], locks[1] remains
- User2 writes → SUCCESS because locks[1] is intact
