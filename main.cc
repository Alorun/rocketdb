#include <iostream>
#include <cassert>

#include "include/db.h"
#include "include/options.h"
#include "include/status.h"

int main() {
    rocketdb::DB* db;
    rocketdb::Options options;
    
    // 关键配置：如果数据库目录不存在则自动创建
    options.create_if_missing = true;

    // 1. 打开数据库 (数据将存在本地的 /tmp/minidb 目录)
    std::string db_path = "/tmp/minidb";
    rocketdb::Status status = rocketdb::DB::Open(options, db_path, &db);
    
    if (!status.ok()) {
        std::cerr << "打开/创建数据库失败: " << status.ToString() << std::endl;
        return -1;
    }
    std::cout << "成功打开数据库: " << db_path << std::endl;

    // 2. 写入数据 (Put)
    std::string key = "hello";
    std::string value = "world_from_my_leveldb";
    status = db->Put(rocketdb::WriteOptions(), key, value);
    assert(status.ok());
    std::cout << "写入成功: " << key << " -> " << value << std::endl;

    // 3. 读取数据 (Get)
    std::string read_value;
    status = db->Get(rocketdb::ReadOptions(), key, &read_value);
    
    if (status.ok()) {
        std::cout << "读取成功: " << key << " -> " << read_value << std::endl;
    } else {
        std::cerr << "未找到该 Key!" << std::endl;
    }

    // 4. 关闭数据库
    delete db;
    return 0;
}