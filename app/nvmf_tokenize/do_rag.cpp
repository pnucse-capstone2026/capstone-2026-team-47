#include "nvmf_tokenize.h"
#include <libpq-fe.h>
#include <string>
#include "httplib.h"
#include "json.hpp"
#include <cstdlib>
#include <cstring>
extern "C" {
#include "tool.h"
}
using json = nlohmann::ordered_json;

const char* do_rag(const char* raw_prompt, const char* postgres_uri) {
  std::string prompt{raw_prompt};

  constexpr auto gemma3_template = "<start_of_turn>user\n";
  auto pos = prompt.rfind(gemma3_template)
    + strlen(gemma3_template);

  const auto user_input = prompt.substr(pos);

  const auto prev_ctx = prompt.substr(0, pos);;
  const json post = {{"content", user_input}};
  httplib::Client cli("localhost", 8080);
  httplib::Result res = cli.Post("/embedding", post.dump(), "application/x-www-form-urlencoded");
  
  if(res.error() != httplib::Error::Success) {
    SPDK_ERRLOG("failed to connect rag server\n");
    ASSERT(false, "connection failure");
  }
		
  if(res->status != 200) {
    SPDK_ERRLOG("failed to connect rag server\n");
    ASSERT(false, "connection failure");
  }

  const auto embedding_result = json::parse(res->body)[0]["embedding"][0];

  PGconn* connection  = PQconnectdb(postgres_uri);
  if(PQstatus(connection) != CONNECTION_OK) {
    SPDK_ERRLOG("failed to connect postgresql: %s\n", PQerrorMessage(connection));
    PQfinish(connection);
    ASSERT(false, "connection failure");
  }

  const auto result_str = embedding_result.dump();
  const char* paramValues[1] = {result_str.c_str()};
  PGresult* pg_res = PQexecParams(connection, "SELECT text FROM vectordb ORDER BY embedding <=> $1 LIMIT 1", 1, NULL, paramValues, NULL, NULL, 0);
  MUST_NONNULL(pg_res, "PGResult is null.");
  char* doc = MUST_NONNULL(PQgetvalue(pg_res, 0, 0), "fetch vector from PGResult failed"); // find just one
		
  std::string ragged_query = prev_ctx + "[reference]" + doc + "[query]" + user_input;
  PQclear(pg_res);
  PQfinish(connection);

  char* ret = (char*)calloc(ragged_query.size() + 1, 1);
  memcpy(ret, ragged_query.data(), ragged_query.size()); 
  return ret;
}
