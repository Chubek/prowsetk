#ifndef PROWSETK_CLI_SERIALIZE_RESOURCES_HPP
#define PROWSETK_CLI_SERIALIZE_RESOURCES_HPP

namespace prowsetk {
class Document;
class Session;

namespace cli {
// Snapshot bounded same-origin resources into a document before IR encoding.
void embed_serialized_resources(Session& session, Document& document);
}
}

#endif
