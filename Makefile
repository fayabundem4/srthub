# Makefile for building srthub

CXX = g++
CXXFLAGS = -Os -Wall -Wextra -std=c++11
LDFLAGS = -lsrt
TARGET = srthub
SRC = srthub.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
