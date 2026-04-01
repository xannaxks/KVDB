#include "main.h"

int main()
{
    try
    {
        Engine engine("wal.log");
        engine.start_up();
        while (true)
        {
            std::string type;
            std::cin >> type;
            if (type == ".exit")
                break;

            if (type == "flush")
            {
                Engine::Status result = engine.manual_freeze();
				std::cout << (result == Engine::Status::OK ? "OK" : "Failed") << std::endl;
                continue;
            }

            std::string key;
            std::cin >> key;

            if (type == "put")
            {
                std::string value;
                std::cin >> value;
                Engine::Status result = engine.put(key, value);
                std::cout << (result == Engine::Status::OK ? "OK" : "Failed") << std::endl;
                continue;
            }

            if (type == "delete")
            {
                Engine::Status result = engine.remove(key);
                std::cout << (result == Engine::Status::OK ? "OK" : result == Engine::Status::KeyNotFound ? "Key was not found" : "Failed") << std::endl;
                continue;
            }

            if (type == "get")
            {
                std::variant<std::string, Engine::Status> result = engine.get(key);
                if (std::holds_alternative<std::string>(result))
                    std::cout << std::get<std::string>(result) << std::endl;
                else if (std::get<Engine::Status>(result) == Engine::Status::KeyNotFound)
                    std::cout << "KeyNotFound" << std::endl;
                else if (std::get<Engine::Status>(result) == Engine::Status::KeyWasDeleted)
                    std::cout << "KeyWasDeleted" << std::endl;
                else
                    std::cout << "Failed" << std::endl;
                continue;
            }
			std::cout << "Unknown command\n";
        }

        return 0;
    }
    catch (const std::bad_alloc&)
    {
        std::cerr << "Fatal: out of memory\n";
        return 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }
    catch (...)
    {
        std::cerr << "Fatal: unknown error\n";
        return 1;
    }
}