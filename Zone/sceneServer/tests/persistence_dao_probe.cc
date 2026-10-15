#include "db/mysqlConnection.h"
#include "db/playerSnapshotDao.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if(argc!=2 || !std::getenv("PERSISTENCE_TEST_PASSWORD"))
            throw std::runtime_error("port and test password environment required");
        MySqlConfig cfg;
        cfg.port=static_cast<uint16_t>(std::stoi(argv[1]));
        cfg.user="persistence_test"; cfg.password=std::getenv("PERSISTENCE_TEST_PASSWORD");
        cfg.dbName="clearmoon_persistence_e2e";
        MySqlConnection db;
        if(!db.Connect(cfg)) throw std::runtime_error("test database connection failed");
        auto load=[&] {
            dbv::DbResult result; db.Execute(PlayerSnapshotDao::buildLoad(4242),result);
            PlayerSaveSnapshot snapshot; std::string error;
            if(!PlayerSnapshotDao::parseLoad(4242,result,snapshot,error)) throw std::runtime_error(error);
            return snapshot;
        };
        const auto before=load();
        auto broken=before;
        ++broken.saveVersion; ++broken.x;
        broken.items.push_back({4294967294u,1,static_cast<uint16_t>(broken.bagNextSlot++)});
        dbv::DbTask task; std::string error;
        if(!PlayerSnapshotDao::buildSave(broken,task,error)) throw std::runtime_error(error);
        dbv::DbResult result; db.Execute(task,result);
        if(!result.errcode || result.classification==dbv::ResultClass::kOutcomeUnknown)
            throw std::runtime_error("expected confirmed foreign-key transaction failure");
        const auto after=load();
        dbv::DbTask expected, actual;
        if(!PlayerSnapshotDao::buildSave(before,expected,error) ||
           !PlayerSnapshotDao::buildSave(after,actual,error) || expected.Encode()!=actual.Encode())
            throw std::runtime_error("partial state persisted after rollback");
        db.Execute(expected,result);
        if(result.errcode || result.classification!=dbv::ResultClass::kSuccess)
            throw std::runtime_error("same-version snapshot replay failed: "+result.errmsg);
        auto older=before;
        if(!older.saveVersion) throw std::runtime_error("test requires nonzero save version");
        --older.saveVersion;
        if(!PlayerSnapshotDao::buildSave(older,task,error)) throw std::runtime_error(error);
        db.Execute(task,result);
        if(result.classification!=dbv::ResultClass::kSuperseded)
            throw std::runtime_error("older snapshot not rejected");
        std::cout << "PASS real DAO: load, rollback, same-version replay, older-version rejection\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "FAIL real DAO: " << e.what() << '\n'; return 1;
    }
}