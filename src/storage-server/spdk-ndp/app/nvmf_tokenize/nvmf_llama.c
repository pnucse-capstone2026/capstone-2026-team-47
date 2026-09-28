#include "nvmf_tokenize.h"

static void llama_log_callback(enum ggml_log_level level, const char *text,
                               void *user_data) {

  static const char *const log_level_str[] = {"NONE", "DEBUG", "INFO",
					      "WARN", "ERROR", "CONT"};
#ifdef NDEBUG
  if (level >= GGML_LOG_LEVEL_ERROR) {
    fprintf(stderr, "[libllama: %s] %s", log_level_str[level], text);
  }
#else
  fprintf(stderr, "[libllama: %s] %s", log_level_str[level], text);
#endif
}

struct tokenizer_ctx *tokenizer_init(const char *model_path,
                                     const char *postgres_uri) {
  
  struct tokenizer_ctx *ret = malloc(sizeof(struct tokenizer_ctx));
  
 /* CAVEAT: it maybe hurts re-entrancy */
  llama_log_set(llama_log_callback, NULL);

  struct llama_model_params model_params = llama_model_default_params();
  model_params.vocab_only = 1;
  
  model_params.no_alloc = 1;
  
  struct llama_model *model =
    llama_model_load_from_file(model_path, model_params);

  const struct llama_vocab *vocab = llama_model_get_vocab(model);
  if (!vocab) {
    fprintf(stderr, "vocab is NULL\n");
    abort();
  }
  const bool model_wants_add_bos = llama_vocab_get_add_bos(vocab);


  *ret = (struct tokenizer_ctx){
      .model = model,
      .vocab = vocab,
      .model_wants_bos = model_wants_add_bos,
      .postgres_uri = postgres_uri,
  };
  
  return ret;
}

int tokenizer_tokenize(struct tokenizer_ctx *ctx, const char *src, uint64_t src_len,
                                  void *dest, uint64_t dest_len) {
  size_t string_len = strlen(src);
  assert(dest_len % sizeof(llama_token) == 0);
  assert(string_len <= src_len);

  size_t output_token_capacity = dest_len / sizeof(llama_token);
  
  int32_t ssize = llama_tokenize(ctx->vocab, src, string_len, dest,
                                 output_token_capacity,
                                 ctx->model_wants_bos, true);
  

  if (ssize == INT32_MIN) {
    SPDK_ERRLOG("tokenize failed, overflow\n");
  } else if (ssize < 0) {
    SPDK_ERRLOG("tokenize failed, %d token should generated.\n", -ssize);
  }
  
  return ssize;
}

void tokenizer_free(struct tokenizer_ctx *ctx) {
  llama_model_free(ctx->model);
  free(ctx);
}

