#include "shared_dict_client.hpp"

#include "utils.hpp"

namespace core {

class SharedDictReader : public SharedDictClient {
public:
    explicit SharedDictReader(CameraConfig config={});
    void read(DataEntry& entry, uint32_t index_from_head=1);

private:
    Buffer* _buffer{nullptr};
};

} // namespace core
