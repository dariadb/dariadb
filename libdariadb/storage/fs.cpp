#include "fs.h"
#include "../exception.h"
#include <iterator>
#include <boost/filesystem.hpp>

namespace dariadb{
    namespace  storage {
        namespace fs {
            std::list<std::string> ls(const std::string &path){
                std::list<boost::filesystem::path> result;
                std::list<std::string> s_result;
                std::copy(boost::filesystem::directory_iterator(path),
                          boost::filesystem::directory_iterator(),
                          std::back_inserter(result));

                for(boost::filesystem::path& it:result){
                    s_result.push_back(it.string());
                }
                return s_result;

            }

            std::list<std::string> ls(const std::string &path, const std::string &ext){
                std::list<boost::filesystem::path> result;
                std::list<std::string> s_result;

                std::copy(boost::filesystem::directory_iterator(path),
                          boost::filesystem::directory_iterator(),
                          std::back_inserter(result));

                // ext filter
                result.remove_if([&ext](boost::filesystem::path p) {
                    return p.extension().string() != ext;
                });

                for(boost::filesystem::path& it:result){
                    s_result.push_back(it.string());
                }
                return s_result;

                return s_result;
            }

            bool rm(const std::string &rm_path){
                if (!boost::filesystem::exists(rm_path))
                    return true;
                try {
                    if (boost::filesystem::is_directory(rm_path)) {
                        boost::filesystem::path path_to_remove(rm_path);
                        for (boost::filesystem::directory_iterator end_dir_it, it(path_to_remove);
                             it != end_dir_it; ++it) {
                            if (!boost::filesystem::remove_all(it->path())) {
                                return false;
                            }
                        }
                    }
                    boost::filesystem::remove_all(rm_path);
                    return true;
                } catch (boost::filesystem::filesystem_error &ex) {
                    std::string msg = ex.what();
                    MAKE_EXCEPTION("utils::rm exception: " + msg);
                    throw;
                }
            }

            std::string filename(std::string fname){ // without ex
                boost::filesystem::path p(fname);
                return p.stem().string();
            }
            std::string parent_path(std::string fname){
                boost::filesystem::path p(fname);

                return p.parent_path().string();

            }

        }
    }
}
