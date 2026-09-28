#include "nvmf_tokenize.h"
#include "spdk/log.h"
/* NOTE: debug/info log can be enabled via rpc.py or cmdline options */

struct extents_info {
  uint64_t nr_extents;
  uint64_t total_nr_blocks;
  
  struct extent {
    uint64_t slba;
    uint64_t nr_blocks;
  } extents[];
};

struct io_ctx {
  struct extents_info *extents_info;
  uint64_t extents_pos;
  uint64_t prev_io_bytes;
  struct iovec buf_head;
};

struct bpe_tokenize_ctx {
  void *raw_extents_data;

  size_t read_buf_size;
  size_t output_buf_size;
  char* read_buf;
  char* output_buf;

  /* common ctxs */
  struct spdk_nvmf_request *req;
  struct spdk_bdev_desc *desc;
  struct spdk_io_channel *ch;
  
  struct spdk_bdev_ext_io_opts opts;

  struct io_ctx read_ctx;
  struct io_ctx write_ctx;

};

/* copied from $(SPDK_ROOT)/lib/nvmf/ctrlr_bdev.c */
static bool
nvmf_bdev_ctrlr_lba_in_range(uint64_t bdev_num_blocks, uint64_t io_start_lba,
			     uint64_t io_num_blocks)
{
	if (io_start_lba + io_num_blocks > bdev_num_blocks ||
	    io_start_lba + io_num_blocks < io_start_lba) {
		return false;
	}

	return true;
}

static bool validate_extents_in_range(struct extents_info *info,
                                      uint64_t bdev_num_blocks,
                                      uint64_t expected_nr_blocks) {
  
  uint64_t sum = 0;
  for(uint64_t i = 0; i < info->nr_extents; ++i) {
    uint64_t start_lba = info->extents[i].slba;
    uint64_t num_blocks = info->extents[i].nr_blocks;
    sum += num_blocks;
    if(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks)) {
      return false;
    }
  }

  return sum == expected_nr_blocks;
}


static void bpe_tokenize_ctx_free(struct bpe_tokenize_ctx* ctx);

static void nvmf_bdev_ctrlr_tokenize_read_complete(struct spdk_bdev_io *bdev_io,
                                                   bool success, void *cb_arg);


static void nvmf_bdev_ctrlr_tokenize_calculate(struct spdk_bdev_io *bdev_io,
                                               bool success, void *cb_arg);

static void nvmf_bdev_ctrlr_tokenize_write_complete(struct spdk_bdev_io *bdev_io,
						    bool success, void *cb_arg);

static void nvmf_bdev_ctrlr_tokenize_complete(struct bpe_tokenize_ctx *ctx);


/* FIXME: extract ctx init logic */
/* entry point */
int nvmf_bdev_ctrlr_tokenize_cmd(struct spdk_bdev *bdev,
                                 struct spdk_bdev_desc *desc,
                                 struct spdk_io_channel *ch,
                                 struct spdk_nvmf_request *req) {
  
  struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
  struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

  const uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
  const uint32_t block_size = spdk_bdev_get_block_size(bdev);

  const uint32_t total_input_bytes = cmd->cdw14 * block_size;
  const uint32_t total_output_bytes = cmd->cdw15 * block_size;

  const size_t input_metadata_bytes =
      sizeof(struct extents_info) + sizeof(struct extent[cmd->cdw12]);

  const size_t output_metadata_bytes =
      sizeof(struct extents_info) + sizeof(struct extent[cmd->cdw13]);

  int rc = 0;


  const size_t metdata_length = input_metadata_bytes + output_metadata_bytes;

  char *raw_extents_info = malloc(metdata_length);

  spdk_copy_iovs_to_buf(/* void* */ raw_extents_info,
                        /* size_t */ metdata_length, req->iov, req->iovcnt);

  struct extents_info *input = (struct extents_info *)&raw_extents_info[cmd->cdw10];
  struct extents_info *output = (struct extents_info *)&raw_extents_info[cmd->cdw11];


  SPDK_ERRLOG("input info : nr_ext %lu, total_blocks %lu\n ",
                input->nr_extents, input->total_nr_blocks);
  SPDK_ERRLOG("output info : nr_ext %lu, total_blocks %lu\n ",
                output->nr_extents, output->total_nr_blocks);

  assert(cmd->cdw14 == input->total_nr_blocks);
  assert(cmd->cdw15 == output->total_nr_blocks);

  /* check input ranges */
  if (!validate_extents_in_range(input, bdev_num_blocks,
                                 total_input_bytes / block_size)) {
      SPDK_ERRLOG("end of media\n");

      rsp->status.sct = SPDK_NVME_SCT_GENERIC;
      rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;

      free(raw_extents_info);
      return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

  /* check output ranges */
  if (!validate_extents_in_range(output, bdev_num_blocks,
                                 total_output_bytes / block_size)) {
      SPDK_ERRLOG("end of media\n");
      rsp->status.sct = SPDK_NVME_SCT_GENERIC;
      rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
      
      free(raw_extents_info);
      return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
  }


  struct bpe_tokenize_ctx *ctx = malloc(sizeof(struct bpe_tokenize_ctx));
  
  // for ease of debugging: size + 1 always ensure null-terminated string
  void *input_buf = spdk_dma_zmalloc(total_input_bytes + 1, 0, NULL);
  void *output_buf = spdk_dma_zmalloc(total_output_bytes + 1, 0, NULL);

  struct spdk_bdev_ext_io_opts opts = {
    .size = SPDK_SIZEOF(&opts, accel_sequence),
    .memory_domain = req->memory_domain,
    .memory_domain_ctx = req->memory_domain_ctx,
    .accel_sequence = req->accel_sequence,
  };

  *ctx = (struct bpe_tokenize_ctx){
      .raw_extents_data = raw_extents_info,
      .req = req,
      .desc = desc,
      .ch = ch,
      .read_buf_size = total_input_bytes,
      .output_buf_size = total_output_bytes,

      .read_buf = input_buf,
      .output_buf = output_buf,
      .opts = opts,

      .read_ctx =
          (struct io_ctx){
              .extents_pos = 0,
              .extents_info = input,
              .buf_head =
                  (struct iovec){
                      .iov_base = input_buf,
                      .iov_len = 0,
                  },
          },

      .write_ctx =
          (struct io_ctx){
              .extents_pos = 0,
              .extents_info = output,
              .buf_head =
                  (struct iovec){
                      .iov_base = output_buf,
                      .iov_len = 0,
                  },
          },

  };

  struct extent *first_ext = &ctx->read_ctx.extents_info->extents[0];

  uint64_t read_blocks = first_ext->nr_blocks;
  uint64_t start_lba = first_ext->slba;

  ctx->read_ctx.buf_head.iov_len = read_blocks * block_size; /* cursed magic */
  ctx->read_ctx.prev_io_bytes = read_blocks * block_size;

  rc = spdk_bdev_readv_blocks_ext(
      desc, ch, &ctx->read_ctx.buf_head, 1, start_lba, read_blocks,
      nvmf_bdev_ctrlr_tokenize_read_complete, ctx, NULL);

  if (rc < 0) {
    SPDK_ERRLOG("initial read submission failed\n");
    bpe_tokenize_ctx_free(ctx);
    return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
  }

  return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}



static void nvmf_bdev_ctrlr_tokenize_read_complete(struct spdk_bdev_io *bdev_io,
                                                   bool success, void *cb_arg) {
  int rc = 0;
  uint32_t cdw0;
  int sct, sc;

  struct bpe_tokenize_ctx *ctx = cb_arg;
  struct spdk_nvmf_request *req = ctx->req;
  struct spdk_bdev_desc* desc = ctx->desc;
  struct spdk_io_channel* ch = ctx->ch;
  struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

  uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
  
  SPDK_ERRLOG( "state: read\n");
  if (!success) {
    spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

    response->cdw0 = cdw0;
    response->status.sc = sc;
    response->status.sct = sct;

    spdk_bdev_free_io(bdev_io);
    bpe_tokenize_ctx_free(ctx);
    return;
  }
  /* note: previous bdev_io must be freed whether it is success or not. */
  spdk_bdev_free_io(bdev_io);
  
  struct io_ctx *read_ctx = &ctx->read_ctx;

  /* advance pointer for dealing with previous read */
  read_ctx->buf_head.iov_base =
      (char *)read_ctx->buf_head.iov_base + read_ctx->prev_io_bytes;
  
  /* -- prepare for next read -- */
  ++read_ctx->extents_pos;
  if (read_ctx->extents_pos == read_ctx->extents_info->nr_extents) {
    SPDK_ERRLOG("state transition\n");
    return nvmf_bdev_ctrlr_tokenize_calculate(NULL, true, ctx);
  }

  assert(read_ctx->extents_pos < read_ctx->extents_info->nr_extents);

  const char *buf_end = ctx->read_buf + ctx->read_buf_size;
  const char *cur_pos =
      (char *)read_ctx->buf_head.iov_base + read_ctx->buf_head.iov_len;

  assert(cur_pos <= buf_end);

  struct extent *ext = &read_ctx->extents_info->extents[read_ctx->extents_pos];

  uint64_t slba = ext->slba;
  uint64_t num_blocks = ext->nr_blocks;
  
  read_ctx->buf_head.iov_len = num_blocks * block_size; /* cursed magic */
  read_ctx->prev_io_bytes = num_blocks * block_size;



  rc = spdk_bdev_readv_blocks_ext(
      desc, ch, &read_ctx->buf_head, 1, slba, num_blocks,
      nvmf_bdev_ctrlr_tokenize_read_complete, ctx, NULL);


  if (rc < 0) {
    SPDK_ERRLOG("read submission failed\n");
    bpe_tokenize_ctx_free(ctx);
  }
}

static void nvmf_bdev_ctrlr_tokenize_calculate(struct spdk_bdev_io *bdev_io,
					       bool success, void *cb_arg) {
  int rc = 0;
  struct bpe_tokenize_ctx *ctx = cb_arg;
  struct spdk_bdev_desc *desc = ctx->desc;
  uint32_t block_size = spdk_bdev_desc_get_block_size(desc);

  struct spdk_io_channel* ch = ctx->ch;

  /* first 4bytes: length of token sequence */

  const char* ragged_prompt = do_rag(ctx->read_buf, g_tokenizer_ctx->postgres_uri);
  
  char *data_start = ctx->output_buf + sizeof(int32_t);
  uint64_t data_size = ctx->output_buf_size - sizeof(int32_t);
  
  assert(data_size < ctx->output_buf_size);

  /* CAVEAT: need proper synchronization */
  int32_t ret = tokenizer_tokenize(g_tokenizer_ctx, ragged_prompt,
                                   strlen(ragged_prompt), data_start, data_size);

  free((void*)ragged_prompt);
  if (ret < 0) {
    SPDK_ERRLOG("tokenizing failed.\n");
    nvmf_bdev_ctrlr_tokenize_complete(ctx);
    return;
  }
  
  SPDK_ERRLOG("tokenizing success, %"PRIi32 " tokens\n", ret);
  /* record data size */
  memcpy(ctx->output_buf, &ret, sizeof(int32_t));
  

  struct extent *first_ext = &ctx->write_ctx.extents_info->extents[0];

  uint64_t start_lba = first_ext->slba;
  uint64_t num_blocks = first_ext->nr_blocks;

  ctx->write_ctx.prev_io_bytes += num_blocks * block_size;
  ctx->write_ctx.buf_head.iov_len = num_blocks * block_size;

  rc = spdk_bdev_writev_blocks_ext(
      desc, ch, &ctx->write_ctx.buf_head, 1, start_lba, num_blocks,
      nvmf_bdev_ctrlr_tokenize_write_complete, ctx, &ctx->opts);

  if (rc < 0) {
    SPDK_ERRLOG("initial write submission failed\n");
    bpe_tokenize_ctx_free(ctx);
  }
}

void nvmf_bdev_ctrlr_tokenize_write_complete(struct spdk_bdev_io *bdev_io,
                                                   bool success, void *cb_arg) {
  int rc = 0;
  uint32_t cdw0;
  int sct, sc;

  struct bpe_tokenize_ctx *ctx = cb_arg;
  struct spdk_nvmf_request *req = ctx->req;
  struct spdk_bdev_desc* desc = ctx->desc;
  struct spdk_io_channel* ch = ctx->ch;
  struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

  uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
  SPDK_ERRLOG("state: write\n");
  if (!success) {
    spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

    response->cdw0 = cdw0;
    response->status.sc = sc;
    response->status.sct = sct;

    spdk_bdev_free_io(bdev_io);
    bpe_tokenize_ctx_free(ctx);
    return;
  }
  
  /* note: previous bdev_io must be freed whether it is success or not. */
  spdk_bdev_free_io(bdev_io);
  
  /* -- prepare for next write -- */
  struct io_ctx *write_ctx = &ctx->write_ctx;
  ++write_ctx->extents_pos;
  if (write_ctx->extents_pos == write_ctx->extents_info->nr_extents) {
      SPDK_ERRLOG("state transition\n");
      nvmf_bdev_ctrlr_tokenize_complete(ctx);
      return;
  }

  assert(write_ctx->extents_pos < write_ctx->extents_info->nr_extents);
  
  struct extent *ext = &write_ctx->extents_info->extents[write_ctx->extents_pos];

  write_ctx->buf_head.iov_base =
      (char *)write_ctx->buf_head.iov_base + write_ctx->prev_io_bytes;

  

  uint64_t slba = ext->slba;
  uint64_t num_blocks = ext->nr_blocks;
  
  write_ctx->buf_head.iov_len = num_blocks * block_size; /* cursed magic */
  ctx->write_ctx.prev_io_bytes = num_blocks * block_size;

  const char *buf_end = ctx->output_buf + ctx->output_buf_size;
  const char *cur_pos =
      (char *)write_ctx->buf_head.iov_base + write_ctx->buf_head.iov_len;

  assert(cur_pos <= buf_end);



  rc = spdk_bdev_writev_blocks_ext(desc, ch, &write_ctx->buf_head, 1, slba,
                                  num_blocks, nvmf_bdev_ctrlr_tokenize_write_complete, ctx,
                                  NULL);

  if (rc < 0) {
    SPDK_ERRLOG("write submission failed\n");
    bpe_tokenize_ctx_free(ctx);
  }
}


void nvmf_bdev_ctrlr_tokenize_complete(struct bpe_tokenize_ctx *ctx) {
  SPDK_ERRLOG("state: complete\n");
  struct spdk_nvmf_request *req = ctx->req;
  struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;


  response->status.sct = SPDK_NVME_SCT_GENERIC;
  response->status.sc = SPDK_NVME_SC_SUCCESS;

  bpe_tokenize_ctx_free(ctx);
}


void bpe_tokenize_ctx_free(struct bpe_tokenize_ctx* ctx) {
  free(ctx->raw_extents_data);
  spdk_free(ctx->read_buf);
  spdk_free(ctx->output_buf);
  spdk_nvmf_request_complete(ctx->req);

  free(ctx);
}

