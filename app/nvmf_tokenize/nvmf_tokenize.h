#pragma once
#ifdef __cplusplus
extern "C" {
#endif
  
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/stdinc.h"
#include "spdk/nvmf.h"
#include "llama.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/nvmf_spec.h"
#include "spdk/nvmf_transport.h"
#include "spdk/trace.h"

struct tokenizer_ctx {
  struct llama_model *model;
  const struct llama_vocab *vocab;
  int model_wants_bos;
  const char* postgres_uri;
};


extern struct tokenizer_ctx *g_tokenizer_ctx;

int nvmf_bdev_ctrlr_tokenize_cmd(struct spdk_bdev *bdev,
                                 struct spdk_bdev_desc *desc,
                                 struct spdk_io_channel *ch,
                                 struct spdk_nvmf_request *req);



const char* do_rag(const char* postgres_uri, const char* raw_prompt);

struct tokenizer_ctx* tokenizer_init(const char *model_path, const char* postgres_uri);
void tokenizer_free(struct tokenizer_ctx * ctx);
int tokenizer_tokenize(struct tokenizer_ctx *ctx, const char *src,
                       uint64_t src_len, void *dest, uint64_t dest_len);

#ifdef __cplusplus
}
#endif
