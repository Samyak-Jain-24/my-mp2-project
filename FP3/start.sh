#!/bin/bash

# LangOS DFS - Quick Start Script
# This script helps you quickly start all components

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}=== LangOS DFS Quick Start ===${NC}\n"

# Check if binaries exist
if [ ! -f "name_server" ] || [ ! -f "storage_server" ] || [ ! -f "client" ]; then
    echo -e "${YELLOW}Binaries not found. Building...${NC}"
    make all
    if [ $? -ne 0 ]; then
        echo -e "${RED}Build failed! Check for errors.${NC}"
        exit 1
    fi
    echo -e "${GREEN}Build successful!${NC}\n"
fi

# Create storage directories
mkdir -p storage1 storage2 storage3

echo "Select what to run:"
echo "1. Name Server"
echo "2. Storage Server (you'll be asked which one)"
echo "3. Client"
echo "4. Complete Demo (opens all in new terminals - requires tmux)"
echo "5. Clean and rebuild"
read -p "Enter choice (1-5): " choice

case $choice in
    1)
        echo -e "${GREEN}Starting Name Server on port 8080...${NC}"
        ./name_server
        ;;
    2)
        echo "Which storage server?"
        echo "1. Storage Server 1 (ports 9001/9101)"
        echo "2. Storage Server 2 (ports 9002/9102)"
        echo "3. Storage Server 3 (ports 9003/9103)"
        read -p "Enter choice (1-3): " ss_choice
        
        case $ss_choice in
            1)
                echo -e "${GREEN}Starting Storage Server 1...${NC}"
                ./storage_server 9001 9101 storage1/
                ;;
            2)
                echo -e "${GREEN}Starting Storage Server 2...${NC}"
                ./storage_server 9002 9102 storage2/
                ;;
            3)
                echo -e "${GREEN}Starting Storage Server 3...${NC}"
                ./storage_server 9003 9103 storage3/
                ;;
            *)
                echo -e "${RED}Invalid choice${NC}"
                exit 1
                ;;
        esac
        ;;
    3)
        echo -e "${GREEN}Starting Client...${NC}"
        ./client
        ;;
    4)
        if ! command -v tmux &> /dev/null; then
            echo -e "${RED}tmux not found. Install with: sudo apt install tmux${NC}"
            echo -e "${YELLOW}Alternatively, open 4 terminals manually:${NC}"
            echo "Terminal 1: ./name_server"
            echo "Terminal 2: ./storage_server 9001 9101 storage1/"
            echo "Terminal 3: ./storage_server 9002 9102 storage2/"
            echo "Terminal 4: ./client"
            exit 1
        fi
        
        echo -e "${GREEN}Starting complete demo in tmux...${NC}"
        echo "Use Ctrl+B then arrow keys to navigate between panes"
        echo "Use Ctrl+B then D to detach from session"
        echo "Use 'tmux attach' to reattach"
        
        # Create tmux session
        tmux new-session -d -s langos
        
        # Split into 4 panes
        tmux split-window -h
        tmux split-window -v
        tmux select-pane -t 0
        tmux split-window -v
        
        # Start components in each pane
        tmux send-keys -t 0 './name_server' C-m
        sleep 1
        tmux send-keys -t 1 './storage_server 9001 9101 storage1/' C-m
        sleep 1
        tmux send-keys -t 2 './storage_server 9002 9102 storage2/' C-m
        sleep 1
        tmux send-keys -t 3 './client' C-m
        
        # Attach to session
        tmux attach-session -t langos
        ;;
    5)
        echo -e "${YELLOW}Cleaning and rebuilding...${NC}"
        make clean
        make all
        echo -e "${GREEN}Done! Run this script again to start components.${NC}"
        ;;
    *)
        echo -e "${RED}Invalid choice${NC}"
        exit 1
        ;;
esac
