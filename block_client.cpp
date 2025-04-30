#include <iostream>
#include <string>
#include <queue>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <stdexcept>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <optional>


struct ParseException : std::runtime_error, rapidjson::ParseResult {
    ParseException(rapidjson::ParseErrorCode code, const char* msg, size_t offset) : 
        std::runtime_error(msg), 
        rapidjson::ParseResult(code, offset) {}
};

#define RAPIDJSON_PARSE_ERROR_NORETURN(code, offset) \
    throw ParseException(code, #code, offset)

#include <rapidjson/document.h>
#include <chrono>

using namespace std;
using namespace rapidjson;

bool debug = false;

// Updated service URL
const string SERVICE_URL = "http://hollywood-graph-crawler.bridgesuncc.org/neighbors/";

// Function to HTTP ecnode parts of URLs. for instance, replace spaces with '%20' for URLs
string url_encode(CURL* curl, string input) {
  char* out = curl_easy_escape(curl, input.c_str(), input.size());
  string s = out;
  curl_free(out);
  return s;
}

// Callback function for writing response data
size_t WriteCallback(void* contents, size_t size, size_t nmemb, string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

// Function to fetch neighbors using libcurl with debugging
string fetch_neighbors(CURL* curl, const string& node) {

    string url = SERVICE_URL + url_encode(curl, node);
    string response;

    if (debug)
      cout << "Sending request to: " << url << endl;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // Verbose Logging

    // Set a User-Agent header to avoid potential blocking by the server
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: C++-Client/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        cerr << "CURL error: " << curl_easy_strerror(res) << endl;
    } else {
      if (debug)
        cout << "CURL request successful!" << endl;
    }

    // Cleanup
    curl_slist_free_all(headers);

    if (debug) 
      cout << "Response received: " << response << endl;  // Debug log

    return (res == CURLE_OK) ? response : "{}";
}

// Function to parse JSON and extract neighbors
vector<string> get_neighbors(const string& json_str) {
    vector<string> neighbors;
    try {
      Document doc;
      doc.Parse(json_str.c_str());
      
      if (doc.HasMember("neighbors") && doc["neighbors"].IsArray()) {
        for (const auto& neighbor : doc["neighbors"].GetArray())
	  neighbors.push_back(neighbor.GetString());
      }
    } catch (const ParseException& e) {
      std::cerr<<"Error while parsing JSON: "<<json_str<<std::endl;
      throw e;
    }
    return neighbors;
}

template <typename T>
class blocking_queue {
  private:
    std::queue<T> queue;
    std::mutex mutex;
    std::condition_variable not_empty;
    bool finished = false;
  public:
    // Push a value to the queue
    void push(const T& value) {
      std::lock_guard<std::mutex> lock(mutex);
      queue.push(value);
      not_empty.notify_one();
    }
    
    // Pop a value from the queue
    std::optional<T> pop() {
      std::unique_lock<std::mutex> lock(mutex);
      // Wait until the queue is not empty or the queue is done
      not_empty.wait(lock, [&] { return finished || !queue.empty(); });
      if(queue.empty()) {
        return std::nullopt;
      }
      T value = queue.front();
      queue.pop();
      return value;
    }
    // Signal that the queue is done
  void signal_done() {
    std::lock_guard<std::mutex> lock(mutex);
    finished = true;
    not_empty.notify_all();
  }
};

// Global variables
blocking_queue<pair<string, int>> work_buffer;
unordered_set<string> visited;
std::vector<string> result;
std::mutex resultMutex, visitedMutex;

// Worker function
void worker(CURL* curl, int depth ) {
  while (true) {
    auto task = work_buffer.pop();
    // If the queue is done, break
    if(!task.has_value()) {
      break;
    }
    auto [node, level] = *task;
    // If the level is less than or equal to the depth, add the node to the result
    if (level <= depth) {
        std::lock_guard<std::mutex> lock(resultMutex);
        result.push_back(node);
    }
    // If the level is less than the depth, fetch neighbors
    if (level < depth) {
      try {
        for (const auto& neighbor : get_neighbors(fetch_neighbors(curl, node))) {
          std::lock_guard<std::mutex> lock(visitedMutex);
          if (visited.insert(neighbor).second) {
            work_buffer.push({neighbor, level + 1});
          }
        }
      } catch (const ParseException& e) {
        std::cerr<<"Error while fetching neighbors of: "<<node<<std::endl;
        throw e;
      }
    }
  }
}

// BFS blocking queue traversal function
std::vector<string> bfs(CURL* curl, const string& start, int depth) {
    std::lock_guard<std::mutex> lock(visitedMutex);
    visited.insert(start);
    work_buffer.push({start, 0});
    
    const int workers = 8;
    std::vector<std::thread> threads;
    // Create worker threads
    for (int i = 0; i < workers; i++) {
        threads.emplace_back(worker, curl, depth);
    }
    // Signal that the queue is done
    work_buffer.signal_done();
    for (auto& thread : threads) {
        thread.join();
    }
    return result;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <node_name> <depth>\n";
        return 1;
    }

    string start_node = argv[1];     // example "Tom%20Hanks"
    int depth;
    try {
        depth = stoi(argv[2]);
    } catch (const exception& e) {
        cerr << "Error: Depth must be an integer.\n";
        return 1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        cerr << "Failed to initialize CURL" << endl;
        return -1;
    }


    const auto start{std::chrono::steady_clock::now()};
    
    
    for (const auto& node : bfs(curl, start_node, depth))
        cout << "- " << node << "\n";

    const auto finish{std::chrono::steady_clock::now()};
    const std::chrono::duration<double> elapsed_seconds{finish - start};
    std::cout << "Time to crawl: "<<elapsed_seconds.count() << "s\n";
    
    curl_easy_cleanup(curl);

    
    return 0;
}
