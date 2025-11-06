#!/bin/bash

# LangOS Distributed File System - Setup and Test Script

echo "=== LangOS DFS Setup Script ==="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    if [ $1 -eq 0 ]; then
        echo -e "${GREEN}✓ $2${NC}"
    else
        echo -e "${RED}✗ $2${NC}"
    fi
}

print_info() {
    echo -e "${YELLOW}→ $1${NC}"
}

# Check dependencies
print_info "Checking dependencies..."

if ! command -v gcc &> /dev/null; then
    print_status 1 "GCC not found. Please install gcc."
    exit 1
fi
print_status 0 "GCC found"

if ! command -v make &> /dev/null; then
    print_status 1 "Make not found. Please install make."
    exit 1
fi
print_status 0 "Make found"

# Clean previous builds
print_info "Cleaning previous builds..."
make cleanall > /dev/null 2>&1
print_status 0 "Cleaned"

# Build the system
print_info "Building the system..."
if make all 2>&1 | grep -q "error"; then
    print_status 1 "Build failed"
    make all
    exit 1
fi
print_status 0 "Build successful"

# Create storage directories
print_info "Creating storage directories..."
mkdir -p storage1 storage2 storage3
print_status 0 "Storage directories created"

# Check if executables exist
print_info "Verifying executables..."
if [ -f "name_server" ] && [ -f "storage_server" ] && [ -f "client" ]; then
    print_status 0 "All executables found"
else
    print_status 1 "Missing executables"
    exit 1
fi

echo ""
echo "=== Setup Complete! ==="
echo ""
echo "To run the system:"
echo ""
echo "Terminal 1 (Name Server):"
echo "  ./name_server"
echo ""
echo "Terminal 2 (Storage Server 1):"
echo "  ./storage_server 9001 9101 storage1/"
echo ""
echo "Terminal 3 (Storage Server 2):"
echo "  ./storage_server 9002 9102 storage2/"
echo ""
echo "Terminal 4+ (Clients):"
echo "  ./client"
echo ""
echo "Or use make targets:"
echo "  make run_nm       # Terminal 1"
echo "  make run_ss1      # Terminal 2"
echo "  make run_ss2      # Terminal 3"
echo "  make run_client   # Terminal 4+"
echo ""

# Offer to create test scripts
read -p "Create test client scripts? (y/n) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    # Create test script 1
    cat > test_client1.sh << 'EOF'
#!/bin/bash
# Test Client 1 - Basic operations

# Send commands to client via stdin
(
sleep 2
echo "user1"
sleep 1
echo "CREATE test.txt"
sleep 1
echo "WRITE test.txt 0"
sleep 1
echo "1 Hello world."
sleep 1
echo "ETIRW"
sleep 1
echo "READ test.txt"
sleep 1
echo "INFO test.txt"
sleep 1
echo "VIEW -l"
sleep 2
echo "EXIT"
) | ./client
EOF
    chmod +x test_client1.sh
    print_status 0 "Created test_client1.sh"
    
    # Create test script 2
    cat > test_client2.sh << 'EOF'
#!/bin/bash
# Test Client 2 - Access control

(
sleep 2
echo "user2"
sleep 2
echo "LIST"
sleep 1
echo "VIEW -a"
sleep 2
echo "EXIT"
) | ./client
EOF
    chmod +x test_client2.sh
    print_status 0 "Created test_client2.sh"
    
    echo ""
    echo "Test scripts created:"
    echo "  ./test_client1.sh - Basic file operations"
    echo "  ./test_client2.sh - Access control test"
fi

echo ""
echo "Setup complete! Ready to launch the system."
