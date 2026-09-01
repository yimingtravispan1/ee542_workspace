CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Iinclude

TARGETS = sender receiver

all: $(TARGETS)

sender: src/sender.cpp include/protocol.h
	$(CXX) $(CXXFLAGS) src/sender.cpp -o sender

receiver: src/receiver.cpp include/protocol.h
	$(CXX) $(CXXFLAGS) src/receiver.cpp -o receiver

clean:
	rm -f $(TARGETS)

.PHONY: all clean