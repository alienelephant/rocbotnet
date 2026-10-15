// The DEMO keys for nodes 0x1234 and 0x1235 (tools/provision.py --demo 1234 1235):
// anyone can derive them from the published test-vector master key. For your
// own network, put your nodes' keys in secrets.h (git-ignored), which this
// sketch prefers. The gateway holds no master key (enrolled mode, SECURITY.md R3).
static const uint8_t NODE_1234_CHAIN_KEY[16] = {0xE3, 0x4F, 0xC4, 0x12, 0xA3, 0x2B, 0x76, 0xA9,
                                                0x28, 0xFE, 0x09, 0xD3, 0xF1, 0x98, 0x95, 0x2D};
static const uint8_t NODE_1235_CHAIN_KEY[16] = {0xC8, 0xA4, 0x52, 0x3D, 0xA4, 0xED, 0x6B, 0xDF,
                                                0x97, 0x06, 0xA2, 0x49, 0x06, 0x95, 0x8F, 0xE7};
