// !! This file has been generated by dnsdist-settings-generator.py, do not edit by hand!!
struct AllSelectorConfiguration;
std::shared_ptr<DNSSelector> getAllSelector(const AllSelectorConfiguration& config);
struct AndSelectorConfiguration;
std::shared_ptr<DNSSelector> getAndSelector(const AndSelectorConfiguration& config);
struct ByNameSelectorConfiguration;
std::shared_ptr<DNSSelector> getByNameSelector(const ByNameSelectorConfiguration& config);
struct DNSSECSelectorConfiguration;
std::shared_ptr<DNSSelector> getDNSSECSelector(const DNSSECSelectorConfiguration& config);
struct DSTPortSelectorConfiguration;
std::shared_ptr<DNSSelector> getDSTPortSelector(const DSTPortSelectorConfiguration& config);
struct EDNSOptionSelectorConfiguration;
std::shared_ptr<DNSSelector> getEDNSOptionSelector(const EDNSOptionSelectorConfiguration& config);
struct EDNSVersionSelectorConfiguration;
std::shared_ptr<DNSSelector> getEDNSVersionSelector(const EDNSVersionSelectorConfiguration& config);
struct ERCodeSelectorConfiguration;
std::shared_ptr<DNSSelector> getERCodeSelector(const ERCodeSelectorConfiguration& config);
struct HTTPHeaderSelectorConfiguration;
std::shared_ptr<DNSSelector> getHTTPHeaderSelector(const HTTPHeaderSelectorConfiguration& config);
struct HTTPPathSelectorConfiguration;
std::shared_ptr<DNSSelector> getHTTPPathSelector(const HTTPPathSelectorConfiguration& config);
struct HTTPPathRegexSelectorConfiguration;
std::shared_ptr<DNSSelector> getHTTPPathRegexSelector(const HTTPPathRegexSelectorConfiguration& config);
struct KeyValueStoreLookupSelectorConfiguration;
std::shared_ptr<DNSSelector> getKeyValueStoreLookupSelector(const KeyValueStoreLookupSelectorConfiguration& config);
struct KeyValueStoreRangeLookupSelectorConfiguration;
std::shared_ptr<DNSSelector> getKeyValueStoreRangeLookupSelector(const KeyValueStoreRangeLookupSelectorConfiguration& config);
struct LuaSelectorConfiguration;
std::shared_ptr<DNSSelector> getLuaSelector(const LuaSelectorConfiguration& config);
struct LuaFFISelectorConfiguration;
std::shared_ptr<DNSSelector> getLuaFFISelector(const LuaFFISelectorConfiguration& config);
struct LuaFFIPerThreadSelectorConfiguration;
std::shared_ptr<DNSSelector> getLuaFFIPerThreadSelector(const LuaFFIPerThreadSelectorConfiguration& config);
struct MaxQPSSelectorConfiguration;
std::shared_ptr<DNSSelector> getMaxQPSSelector(const MaxQPSSelectorConfiguration& config);
struct MaxQPSIPSelectorConfiguration;
std::shared_ptr<DNSSelector> getMaxQPSIPSelector(const MaxQPSIPSelectorConfiguration& config);
struct NetmaskGroupSelectorConfiguration;
std::shared_ptr<DNSSelector> getNetmaskGroupSelector(const NetmaskGroupSelectorConfiguration& config);
struct NotSelectorConfiguration;
std::shared_ptr<DNSSelector> getNotSelector(const NotSelectorConfiguration& config);
struct OpcodeSelectorConfiguration;
std::shared_ptr<DNSSelector> getOpcodeSelector(const OpcodeSelectorConfiguration& config);
struct OrSelectorConfiguration;
std::shared_ptr<DNSSelector> getOrSelector(const OrSelectorConfiguration& config);
struct PayloadSizeSelectorConfiguration;
std::shared_ptr<DNSSelector> getPayloadSizeSelector(const PayloadSizeSelectorConfiguration& config);
struct PoolAvailableSelectorConfiguration;
std::shared_ptr<DNSSelector> getPoolAvailableSelector(const PoolAvailableSelectorConfiguration& config);
struct PoolOutstandingSelectorConfiguration;
std::shared_ptr<DNSSelector> getPoolOutstandingSelector(const PoolOutstandingSelectorConfiguration& config);
struct ProbaSelectorConfiguration;
std::shared_ptr<DNSSelector> getProbaSelector(const ProbaSelectorConfiguration& config);
struct ProxyProtocolValueSelectorConfiguration;
std::shared_ptr<DNSSelector> getProxyProtocolValueSelector(const ProxyProtocolValueSelectorConfiguration& config);
struct QClassSelectorConfiguration;
std::shared_ptr<DNSSelector> getQClassSelector(const QClassSelectorConfiguration& config);
struct QNameSelectorConfiguration;
std::shared_ptr<DNSSelector> getQNameSelector(const QNameSelectorConfiguration& config);
struct QNameLabelsCountSelectorConfiguration;
std::shared_ptr<DNSSelector> getQNameLabelsCountSelector(const QNameLabelsCountSelectorConfiguration& config);
struct QNameSetSelectorConfiguration;
std::shared_ptr<DNSSelector> getQNameSetSelector(const QNameSetSelectorConfiguration& config);
struct QNameSuffixSelectorConfiguration;
std::shared_ptr<DNSSelector> getQNameSuffixSelector(const QNameSuffixSelectorConfiguration& config);
struct QNameWireLengthSelectorConfiguration;
std::shared_ptr<DNSSelector> getQNameWireLengthSelector(const QNameWireLengthSelectorConfiguration& config);
struct QTypeSelectorConfiguration;
std::shared_ptr<DNSSelector> getQTypeSelector(const QTypeSelectorConfiguration& config);
struct RCodeSelectorConfiguration;
std::shared_ptr<DNSSelector> getRCodeSelector(const RCodeSelectorConfiguration& config);
struct RDSelectorConfiguration;
std::shared_ptr<DNSSelector> getRDSelector(const RDSelectorConfiguration& config);
struct RE2SelectorConfiguration;
std::shared_ptr<DNSSelector> getRE2Selector(const RE2SelectorConfiguration& config);
struct RecordsCountSelectorConfiguration;
std::shared_ptr<DNSSelector> getRecordsCountSelector(const RecordsCountSelectorConfiguration& config);
struct RecordsTypeCountSelectorConfiguration;
std::shared_ptr<DNSSelector> getRecordsTypeCountSelector(const RecordsTypeCountSelectorConfiguration& config);
struct RegexSelectorConfiguration;
std::shared_ptr<DNSSelector> getRegexSelector(const RegexSelectorConfiguration& config);
struct SNISelectorConfiguration;
std::shared_ptr<DNSSelector> getSNISelector(const SNISelectorConfiguration& config);
struct TagSelectorConfiguration;
std::shared_ptr<DNSSelector> getTagSelector(const TagSelectorConfiguration& config);
struct TCPSelectorConfiguration;
std::shared_ptr<DNSSelector> getTCPSelector(const TCPSelectorConfiguration& config);
struct TrailingDataSelectorConfiguration;
std::shared_ptr<DNSSelector> getTrailingDataSelector(const TrailingDataSelectorConfiguration& config);
