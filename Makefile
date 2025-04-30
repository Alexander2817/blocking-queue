#CXXFLAGS=-I ~/prgs/rapidjson/include
CXX = g++
CXXFLAGS = -std=c++17 -Wall
LDFLAGS = -lcurl -pthread

all: client block_client

client: client.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

block_client: block_client.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	-rm client client.o block_client block_client.o
