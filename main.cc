#include <iostream>
#include <string>

#include "include/db.h"

int main() {
    rocketdb::DB* db;
    rocketdb::Options options;
    options.create_if_missing = true;

    rocketdb::Status status = rocketdb::DB::Open(options, "./mydb", &db);
    
    if (!status.ok()) {
        std::cerr << "Open rocketdb failed!" << std::endl;
        return -1;
    }

    std::cout << "rocketDB started successfully! Please enter the command." << std::endl
              << "(Example: put k1 v1, get k1, exit)" << std::endl;

    std::string cmd, key, value;

    while (std::cin >> cmd) {
        if (cmd == "put") {
            std::cin >> key >> value;
            db->Put(rocketdb::WriteOptions(), key, value);
            std::cout << "-> OK" << std::endl;
        } 
        else if (cmd == "get") {
            std::cin >> key;
            status = db->Get(rocketdb::ReadOptions(), key, &value);
            if (status.ok()) std::cout << "-> " << value << std::endl;
            else std::cout << "-> (Not Found)" << std::endl;
        } 
        else if (cmd == "exit") {
            break; 
        } 
        else {
            std::cout << "Woring cmd" << std::endl;
        }
    }

    delete db;
    return 0;
}