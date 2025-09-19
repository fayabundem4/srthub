# Makefile for building srthub

CXX = cc
CXXFLAGS = -Os -Wall -Wextra -pedantic -std=gnu99
LDFLAGS = -lsrt 
TARGET = srthub
SRC = srthub.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
