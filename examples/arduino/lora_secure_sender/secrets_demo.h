// The DEMO key for node 0x1234 (tools/provision.py --demo 1234): anyone can
// derive it from the published test-vector master key. For your own network,
// put your node's key in secrets.h (git-ignored), which this sketch prefers:
//   python3 tools/provision.py --master <your master> 1234
static const uint8_t NODE_1234_CHAIN_KEY[16] = {0xE3, 0x4F, 0xC4, 0x12, 0xA3, 0x2B, 0x76, 0xA9,
                                                0x28, 0xFE, 0x09, 0xD3, 0xF1, 0x98, 0x95, 0x2D};
