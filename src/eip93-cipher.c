/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2019 - 2020
 *
 * Richard van Schagen <vschagen@cs.com>
 */
//#define DEBUG 1
#include <crypto/aead.h>
#include <crypto/aes.h>
#include <crypto/authenc.h>
#include <crypto/ctr.h>
#include <crypto/hmac.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/des.h>
#include <crypto/internal/skcipher.h>
#include <crypto/md5.h>
#include <crypto/null.h>
#include <crypto/scatterwalk.h>
#include <crypto/sha.h>

#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

#include "eip93-common.h"
#include "eip93-core.h"
#include "eip93-cipher.h"
#include "eip93-regs.h"
#include "eip93-ring.h"

inline void mtk_free_sg_cpy(const int len, struct scatterlist **sg)
{
	if (!*sg || !len)
		return;

	free_pages((unsigned long)sg_virt(*sg), get_order(len));
	kfree(*sg);
	*sg = NULL;
}

inline int mtk_make_sg_cpy(struct scatterlist *src, struct scatterlist **dst,
		const int len, struct mtk_cipher_reqctx *rctx, const bool copy)
{
	void *pages;
	int totallen;

	*dst = kmalloc(sizeof(**dst), GFP_KERNEL);
	if (!*dst) {
		printk("NO MEM\n");
		return -ENOMEM;
	}
	/* allocate enough memory for full scatterlist */
	totallen = rctx->assoclen + rctx->textsize + rctx->authsize;

	pages = (void *)__get_free_pages(GFP_KERNEL | GFP_DMA,
					get_order(totallen));
	if (!pages) {
		kfree(*dst);
		*dst = NULL;
		printk("no free pages\n");
		return -ENOMEM;
	}

	sg_init_table(*dst, 1);
	sg_set_buf(*dst, pages, totallen);
	/* copy only as requested */
	if (copy)
		sg_copy_to_buffer(src, sg_nents(src), pages, len);

	return 0;
}

inline bool mtk_is_sg_aligned(struct scatterlist *sg, u32 len, const int blksz)
{
	int nents;

	for (nents = 0; sg; sg = sg_next(sg), ++nents) {
		/* When destination buffers are not aligned to the cache line
		 * size we need bounce buffers. The DMA-API requires that the
		 * entire line is owned by the DMA buffer.
		 */
		if (!IS_ALIGNED(sg->offset, 32))
			return false;

		/* segments need to be blocksize aligned */
		if (len <= sg->length) {
			if (!IS_ALIGNED(len, blksz))
				return false;

			return true;
		}

		if (!IS_ALIGNED(sg->length, blksz))
			return false;

		len -= sg->length;
	}
	return false;
}

inline void mtk_ctx_saRecord(struct mtk_cipher_ctx *ctx, const u8 *key,
				const u32 nonce, const unsigned int keylen,
				const unsigned long int flags)
{
	struct saRecord_s *saRecord;

	saRecord = ctx->sa;
	/*
	 * Load and Save IV in saState and set Basic operation
	 */
	saRecord->saCmd0.bits.ivSource = 2;
	saRecord->saCmd0.bits.saveIv = 1 ;
	saRecord->saCmd0.bits.opGroup = 0;
	saRecord->saCmd0.bits.opCode = 0;

	saRecord->saCmd0.bits.cipher = 15;
	switch ((flags & MTK_ALG_MASK)) {
	case MTK_ALG_AES:
		saRecord->saCmd0.bits.cipher = 3;
		saRecord->saCmd1.bits.aesKeyLen = (keylen / 8);
		break;
	case MTK_ALG_3DES:
		saRecord->saCmd0.bits.cipher = 1;
		break;
	case MTK_ALG_DES:
		saRecord->saCmd0.bits.cipher = 0;
		break;
	}

	saRecord->saCmd0.bits.saveHash = 1;

	switch ((flags & MTK_HASH_MASK)) {
	case MTK_HASH_SHA256:
		saRecord->saCmd0.bits.hash = 3;
		break;
	case MTK_HASH_SHA224:
		saRecord->saCmd0.bits.hash = 2;
		break;
	case MTK_HASH_SHA1:
		saRecord->saCmd0.bits.hash = 1;
		break;
	case MTK_HASH_MD5:
		saRecord->saCmd0.bits.hash = 0;
		break;
	default:
		saRecord->saCmd0.bits.saveHash = 0;
		saRecord->saCmd0.bits.hash = 15;
	}

	saRecord->saCmd0.bits.hdrProc = 0;
	saRecord->saCmd0.bits.padType = 3;
	saRecord->saCmd0.bits.extPad = 0;
	saRecord->saCmd0.bits.scPad = 0;

	switch ((flags & MTK_MODE_MASK)) {
	case MTK_MODE_ECB:
		saRecord->saCmd1.bits.cipherMode = 0;
		break;
	case MTK_MODE_CBC:
		saRecord->saCmd1.bits.cipherMode = 1;
		break;
	case MTK_MODE_CTR:
		saRecord->saCmd1.bits.cipherMode = 2;
		break;
	}

	saRecord->saCmd1.bits.byteOffset = 0;
	saRecord->saCmd1.bits.hashCryptOffset = 0;
	saRecord->saCmd0.bits.digestLength = 0;
	saRecord->saCmd1.bits.copyPayload = 0;

	if (IS_HMAC(flags)) {
		saRecord->saCmd1.bits.hmac = 1;
		saRecord->saCmd1.bits.copyDigest = 1;
		saRecord->saCmd1.bits.copyHeader = 1;
	} else {
		saRecord->saCmd1.bits.hmac = 0;
		saRecord->saCmd1.bits.copyDigest = 0;
		saRecord->saCmd1.bits.copyHeader = 0;
	}

	memcpy(saRecord->saKey, key, keylen);

	if (IS_RFC3686(flags))
		saRecord->saNonce = nonce;

	/* Default for now, might be used for ESP offload */
	saRecord->saCmd1.bits.seqNumCheck = 0;
	saRecord->saSpi = 0x0;
	saRecord->saSeqNumMask[0] = 0xFFFFFFFF;
	saRecord->saSeqNumMask[1] = 0x0;
}

/*
 * Poor mans Scatter/gather function:
 * Create a Descriptor for every segment to avoid copying buffers.
 * For performance better to wait for hardware to perform multiple DMA
 *
 */
inline int mtk_scatter_combine(struct mtk_device *mtk, dma_addr_t saRecord_base,
			dma_addr_t saState_base, struct scatterlist *sgsrc,
			struct scatterlist *sgdst, u32 datalen,  bool complete,
			unsigned int *areq, int *commands, int *results)
{
	struct mtk_desc_buf *buf;
	unsigned int remainin, remainout;
	int offsetin = 0, offsetout = 0;
	u32 n, len;
	dma_addr_t saddr, daddr;
	bool nextin = false;
	bool nextout = false;
	struct eip93_descriptor_s *cdesc;
	struct eip93_descriptor_s *rdesc;
	int ndesc_cdr = 0, ndesc_rdr = 0;
	int wptr, saPointer;

	n = datalen;
	remainin = min(sg_dma_len(sgsrc), n);
	remainout = min(sg_dma_len(sgdst), n);
	saddr = sg_dma_address(sgsrc);
	daddr = sg_dma_address(sgdst);
	saPointer = mtk_ring_curr_wptr_index(mtk);

	do {
		if (nextin) {
			sgsrc = sg_next(sgsrc);
			remainin = min(sg_dma_len(sgsrc), n);
			if (remainin == 0)
				continue;

			saddr = sg_dma_address(sgsrc);
			offsetin = 0;
			nextin = false;
		}

		if (nextout) {
			sgdst = sg_next(sgdst);
			remainout = min(sg_dma_len(sgdst), n);
			if (remainout == 0)
				continue;

			daddr = sg_dma_address(sgdst);
			offsetout = 0;
			nextout = false;
		}
		if (remainin == remainout) {
			len = remainin;
				nextin = true;
				nextout = true;
		} else if (remainin < remainout) {
			len = remainin;
				offsetout += len;
				remainout -= len;
				nextin = true;
		} else {
			len = remainout;
				offsetin += len;
				remainin -= len;
				nextout = true;
		}

		rdesc = mtk_add_rdesc(mtk, &wptr);
		if (IS_ERR(rdesc))
			dev_err(mtk->dev, "No RDR mem");

		cdesc = mtk_add_cdesc(mtk, &wptr);
		if (IS_ERR(cdesc))
			dev_err(mtk->dev, "No CDR mem");

		cdesc->peCrtlStat.bits.hostReady = 1;
		cdesc->peCrtlStat.bits.prngMode = 0;
		cdesc->peCrtlStat.bits.hashFinal = 1;
		cdesc->peCrtlStat.bits.padCrtlStat = 0;
		cdesc->peCrtlStat.bits.peReady = 0;
		cdesc->srcAddr = saddr + offsetin;
		cdesc->dstAddr = daddr + offsetout;
		cdesc->saAddr = saRecord_base;
		cdesc->stateAddr = saState_base;
		cdesc->arc4Addr = saState_base;
		cdesc->userId = 0;
		cdesc->peLength.bits.byPass = 0;
		cdesc->peLength.bits.length = len;
		cdesc->peLength.bits.hostReady = 1;
		buf = &mtk->ring[0].dma_buf[wptr];
		buf->flags = MTK_DESC_ASYNC;
		buf->req = areq;
		buf->saPointer = saPointer;
		ndesc_cdr++;
		ndesc_rdr++;
		n -= len;
	} while (n);

	/*
	 * for skcipher and AEAD complete indicates
	 * LAST -> all segments have been processed: unmap_dma
	 * FINISH -> complete the requests
	 */
	 buf->flags |= MTK_DESC_LAST;

	 if (complete == true)
		buf->flags |= MTK_DESC_FINISH;

	*commands = ndesc_cdr;
	*results = ndesc_rdr;

	return 0;
}

inline int mtk_send_req(struct crypto_async_request *base,
		const struct mtk_cipher_ctx *ctx,
		struct scatterlist *reqsrc, struct scatterlist *reqdst,
		const u8 *reqiv, struct mtk_cipher_reqctx *rctx,
		int *commands, int *results)
{
	struct mtk_device *mtk = ctx->mtk;
	int ndesc_cdr = 0, ndesc_rdr = 0, ctr_cdr = 0, ctr_rdr = 0;
	int offset = 0, err, wptr;
	u32 aad = rctx->assoclen;
	u32 textsize = rctx->textsize;
	u32 authsize = rctx->authsize;
	u32 datalen = aad + textsize;
	u32 totlen_src = datalen;
	u32 totlen_dst = datalen;
	struct scatterlist *src, *src_ctr;
	struct scatterlist *dst, *dst_ctr;
	struct saRecord_s *saRecord;
	struct saState_s *saState;
	dma_addr_t saState_base, saRecord_base;
	u32 start, end, ctr, blocks;
	unsigned long int flags = rctx->flags;
	bool overflow;
	bool complete = true;
	bool src_align = true, dst_align = true;
	u32 iv[AES_BLOCK_SIZE / sizeof(u32)];
	int blksize = 1;

	switch ((flags & MTK_ALG_MASK))	{
	case MTK_ALG_AES:
		blksize = AES_BLOCK_SIZE;
		break;
	case MTK_ALG_DES:
		blksize = DES_BLOCK_SIZE;
		break;
	case MTK_ALG_3DES:
		blksize = DES3_EDE_BLOCK_SIZE;
		break;
	}

	if (ctx->aead) {
		if (IS_ENCRYPT(flags))
			totlen_dst += authsize;
		else
			totlen_src += authsize;
	}

	if (!IS_CTR(rctx->flags)) {
		if (!IS_ALIGNED(textsize, blksize))
			return -EINVAL;
	}

	rctx->sg_src = NULL;
	src = reqsrc;
	rctx->sg_dst = NULL;
	dst = reqdst;

	rctx->src_nents = sg_nents_for_len(src, totlen_src);
	rctx->dst_nents = sg_nents_for_len(dst, totlen_dst);

	if (src == dst) {
		rctx->src_nents = max(rctx->src_nents, rctx->dst_nents);
		rctx->dst_nents = rctx->src_nents;
		if (unlikely((totlen_src || totlen_dst) &&
		    (rctx->src_nents <= 0))) {
			dev_err(mtk->dev, "In-place buffer not large enough (need %d bytes)!",
				max(totlen_src, totlen_dst));
			return -EINVAL;
		}
	} else {
		if (unlikely(totlen_src && (rctx->src_nents <= 0))) {
			dev_err(mtk->dev, "Source buffer not large enough (need %d bytes)!",
				totlen_src);
			return -EINVAL;
		}

		if (unlikely(totlen_dst && (rctx->dst_nents <= 0))) {
			dev_err(mtk->dev, "Dest buffer not large enough (need %d bytes)!",
				totlen_dst);
			return -EINVAL;
		}
	}

	if (ctx->aead) {
		src_align = false;
		dst_align = false;
	} else {
		src_align = mtk_is_sg_aligned(src, totlen_src, blksize);
		dst_align = mtk_is_sg_aligned(reqdst, totlen_dst, blksize);
	}

	if (!src_align) {
		rctx->sg_src = reqsrc;
		err = mtk_make_sg_cpy(rctx->sg_src, &rctx->sg_src,
					totlen_src, rctx, true);
		if (err)
			return err;
		src = rctx->sg_src;
	}

	if (!dst_align) {
		rctx->sg_dst = reqdst;
		err = mtk_make_sg_cpy(rctx->sg_dst, &rctx->sg_dst,
					totlen_dst, rctx, false);
		if (err)
			return err;
		dst = rctx->sg_dst;
	}

	/* map DMA_BIDIRECTIONAL to invalidate cache on destination */
	dma_map_sg(mtk->dev, dst, sg_nents(dst), DMA_BIDIRECTIONAL);
	if (src != dst)
		dma_map_sg(mtk->dev, src, sg_nents(src), DMA_TO_DEVICE);

	if (IS_CBC(flags) || IS_CTR(flags))
		memcpy(iv, reqiv, AES_BLOCK_SIZE);

	overflow = (IS_CTR(rctx->flags)  && (!IS_RFC3686(rctx->flags)));

	if (overflow) {
		/* Compute data length. */
		blocks = DIV_ROUND_UP(totlen_src, AES_BLOCK_SIZE);
		ctr = be32_to_cpu(iv[3]);
		/* Check 32bit counter overflow. */
		start = ctr;
		end = start + blocks - 1;
		if (end < start) {
			offset = AES_BLOCK_SIZE * -start;
			/*
		 	* Increment the counter manually to cope with
		 	* the hardware counter overflow.
		 	*/
			ctr = 0xffffffff;
			iv[3] = cpu_to_be32(ctr);
			crypto_inc((u8 *)iv, AES_BLOCK_SIZE);
			complete = false;
		}
	}
	/*
	 * Keep all descriptors off 1 request together with desc_lock
	 * TODO: rethink the logic of handling results.
	 */
	spin_lock(&mtk->ring[0].desc_lock);

	wptr = mtk_ring_curr_wptr_index(mtk);
	saState = &mtk->saState[wptr];
	saState_base = mtk->saState_base + wptr * sizeof(saState_t);
	saRecord = &mtk->saRecord[wptr];
	saRecord_base = mtk->saRecord_base + wptr * sizeof(saRecord_t);
	memcpy(saRecord, ctx->sa, sizeof(struct saRecord_s));

	if (IS_DECRYPT(flags))
		saRecord->saCmd0.bits.direction = 1;

	if (ctx->aead) {
		saRecord->saCmd0.bits.opCode = 1;
	}
/*
	if (IS_GENIV(flags)) {
		printk("geniv");
		saRecord->saCmd0.bits.opCode = 0;
		saRecord->saCmd0.bits.opGroup = 1;
		saRecord->saCmd1.bits.hashCryptOffset = (rctx->ivsize / 4);
		if (IS_ENCRYPT(flags))
			saRecord->saCmd0.bits.ivSource = 3;
		else
			saRecord->saCmd0.bits.ivSource = 1;
	}
*/
	if (IS_HMAC(flags)) {
		saRecord->saCmd1.bits.hashCryptOffset = (aad / 4);
		saRecord->saCmd0.bits.digestLength = (authsize / 4);
	}

	if (IS_CBC(flags) || overflow)
		memcpy(saState->stateIv, reqiv, AES_BLOCK_SIZE);
	else if (IS_RFC3686(flags)) {
		saState->stateIv[0] = ctx->sa->saNonce;
		saState->stateIv[1] = iv[0];
		saState->stateIv[2] = iv[1];
		saState->stateIv[3] = cpu_to_be32(1);
	}

	/* TODO: check logic for wptr in case multiple requests */
	if (unlikely(complete == false)) {
		src_ctr = src;
		dst_ctr = dst;
		err = mtk_scatter_combine(mtk, saRecord_base,
				saState_base, src, dst,
				offset, complete, (void *)base,
				&ctr_cdr, &ctr_rdr);
		/* Jump to offset. */
		src = scatterwalk_ffwd(rctx->ctr_src, src_ctr, offset);
		dst = ((src_ctr == dst_ctr) ? src :
			scatterwalk_ffwd(rctx->ctr_dst, dst_ctr, offset));
		/* Set new State */
		wptr = mtk_ring_curr_wptr_index(mtk);
		saState = &mtk->saState[wptr];
		saState_base = mtk->saState_base + wptr * sizeof(saState_t);
		memcpy(saState->stateIv, iv, AES_BLOCK_SIZE);
		datalen -= offset;
		complete = true;
		/* map DMA_BIDIRECTIONAL to invalidate cache on destination */
		dma_map_sg(mtk->dev, dst, sg_nents(dst), DMA_BIDIRECTIONAL);
		if (src != dst)
			dma_map_sg(mtk->dev, src, sg_nents(src), DMA_TO_DEVICE);
	}

	err = mtk_scatter_combine(mtk, saRecord_base,
			saState_base, src, dst,
			datalen, complete, (void *)base,
			&ndesc_cdr, &ndesc_rdr);

	spin_unlock(&mtk->ring[0].desc_lock);

	*commands = ndesc_cdr + ctr_cdr;
	*results = ndesc_rdr + ctr_rdr;

	return 0;
}

inline int mtk_req_result(struct mtk_device *mtk, struct mtk_cipher_reqctx *rctx,
		struct scatterlist *reqsrc, struct scatterlist *reqdst,
		u8 *reqiv, bool *should_complete, int *ret)
{
	struct eip93_descriptor_s *cdesc;
	struct eip93_descriptor_s *rdesc;
	struct mtk_desc_buf *buf;
	struct crypto_async_request *req = NULL;
	struct saState_s *saState;
	u32 saPointer;
	int ndesc = 0, rptr = 0, nreq;
	int try, i;
	volatile int done1, done2;
	bool last_entry = false;
	u32 aad = rctx->assoclen;
	u32 len = aad + rctx->textsize;
	u32 authsize = rctx->authsize;
	u32 auth = 0;
	u32 *otag;

	*ret = 0;
	*should_complete = false;

	nreq = readl(mtk->base + EIP93_REG_PE_RD_COUNT) & GENMASK(10, 0);

	spin_lock(&mtk->ring[0].rdesc_lock);
	while (ndesc < nreq) {
		rdesc = mtk_ring_next_rptr(mtk, &mtk->ring[0].rdr, &rptr);
		if (IS_ERR(rdesc)) {
			dev_err(mtk->dev, "Ndesc: %d nreq: %d\n", ndesc, nreq);
			*ret = PTR_ERR(rdesc);
			break;
		}
		/* make sure EIP93 finished writing all data
		 * (volatile int) used since bits will be updated via DMA
		*/
		try = 0;
		while (try < 1000) {
			done1 = (volatile int)rdesc->peCrtlStat.bits.peReady;
			done2 = (volatile int)rdesc->peLength.bits.peReady;
			if ((!done1) || (!done2)) {
					try++;
					cpu_relax();
					continue;
			}
			break;
		}
		/*
		if (try)
			dev_err(mtk->dev, "EIP93 try-count: %d", try);
		*/

		if (rdesc->peCrtlStat.bits.errStatus) {
			dev_err(mtk->dev, "Err: %02x\n",
					rdesc->peCrtlStat.bits.errStatus);
			*ret = -EINVAL;
		}

		cdesc = mtk_ring_next_rptr(mtk, &mtk->ring[0].cdr, &rptr);
		if (IS_ERR(cdesc)) {
			dev_err(mtk->dev, "Cant get Cdesc");
			*ret = PTR_ERR(cdesc);
			break;
		}

		buf = &mtk->ring[0].dma_buf[rptr];
		if (buf->flags & MTK_DESC_FINISH)
			*should_complete = true;
		if (buf->flags & MTK_DESC_LAST)
			last_entry = true;
		buf->flags = 0;
		ndesc++;
		if (last_entry)
			break;
	}
	spin_unlock(&mtk->ring[0].rdesc_lock);

	if (!last_entry)
		return ndesc;

	if (!rctx->sg_src && !rctx->sg_dst && reqsrc == reqdst) {
		dma_unmap_sg(mtk->dev, reqdst, rctx->dst_nents,
			DMA_BIDIRECTIONAL);
		goto update_iv;
	}

	if (rctx->sg_src) {
		dma_unmap_sg(mtk->dev, rctx->sg_src,
			sg_nents(rctx->sg_src), DMA_TO_DEVICE);
		mtk_free_sg_cpy(len + authsize, &rctx->sg_src);
	} else
		dma_unmap_sg(mtk->dev, reqsrc, sg_nents(reqsrc),
				DMA_TO_DEVICE);

	if (rctx->sg_dst) {
		dma_unmap_sg(mtk->dev, rctx->sg_dst,
			sg_nents(rctx->sg_dst), DMA_FROM_DEVICE);
		/* EIP93 Little endian MD5; Big Endian all SHA */
		if (authsize) {
			if (!IS_HASH_MD5(rctx->flags)) {
				otag = sg_virt(rctx->sg_dst) + len;
				for (i = 0; i < (authsize / 4); i++)
					otag[i] = ntohl(otag[i]);
			}
		}
		if (IS_ENCRYPT(rctx->flags))
			auth = authsize;

		sg_copy_from_buffer(reqdst, sg_nents(reqdst),
				sg_virt(rctx->sg_dst), len + auth);
		mtk_free_sg_cpy(len + authsize, &rctx->sg_dst);
	} else
		dma_unmap_sg(mtk->dev, reqdst, sg_nents(reqdst),
					DMA_FROM_DEVICE);

	if (!*should_complete)
		return ndesc;

	/* API expects updated IV for CBC and CTR (no RFC3686) */
update_iv:
	if ((!IS_RFC3686(rctx->flags)) &&
		(IS_CBC(rctx->flags) || IS_CTR(rctx->flags))) {
		saPointer = buf->saPointer;
		saState = &mtk->saState[saPointer];
		memcpy(reqiv, saState->stateIv, rctx->ivsize);
	}

	if (IS_BUSY(rctx->flags)) {
		req = (struct crypto_async_request *)buf->req;
		local_bh_disable();
		req->complete(req, -EINPROGRESS);
		local_bh_enable();
	}

	return ndesc;
}

int mtk_skcipher_handle_result(struct mtk_device *mtk,
				struct crypto_async_request *async,
				bool *should_complete,  int *ret)
{
	struct skcipher_request *req = skcipher_request_cast(async);
	struct mtk_cipher_reqctx *rctx = skcipher_request_ctx(req);

	return mtk_req_result(mtk, rctx, req->src, req->dst, req->iv,
				should_complete, ret);
}

int mtk_aead_handle_result(struct mtk_device *mtk,
				struct crypto_async_request *async,
				bool *should_complete,  int *ret)
{
	struct aead_request *req = aead_request_cast(async);
	struct mtk_cipher_reqctx *rctx = aead_request_ctx(req);

	return mtk_req_result(mtk, rctx, req->src, req->dst, req->iv,
				should_complete, ret);
}

/* Crypto skcipher API functions */
static int mtk_skcipher_cra_init(struct crypto_tfm *tfm)
{
	struct mtk_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	struct mtk_alg_template *tmpl = container_of(tfm->__crt_alg,
				struct mtk_alg_template, alg.skcipher.base);

	memset(ctx, 0, sizeof(*ctx));

	crypto_skcipher_set_reqsize(__crypto_skcipher_cast(tfm),
				sizeof(struct mtk_cipher_reqctx));

	ctx->mtk = tmpl->mtk;
	ctx->base.handle_result = mtk_skcipher_handle_result;
	ctx->aead = false;
	ctx->sa = kzalloc(sizeof(struct saRecord_s), GFP_KERNEL);
	if (!ctx->sa)
		printk("!! no sa memory\n");

	ctx->fallback = crypto_alloc_sync_skcipher(crypto_tfm_alg_name(tfm), 0,
				CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK);

	if (IS_ERR(ctx->fallback))
		ctx->fallback = NULL;

	return 0;
}

static void mtk_skcipher_cra_exit(struct crypto_tfm *tfm)
{
	struct mtk_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	kfree(ctx->sa);

	if (ctx->fallback)
		crypto_free_sync_skcipher(ctx->fallback);
}

static int mtk_skcipher_setkey(struct crypto_skcipher *ctfm, const u8 *key,
				 unsigned int len)
{
	struct crypto_tfm *tfm = crypto_skcipher_tfm(ctfm);
	struct mtk_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	struct mtk_alg_template *tmpl = container_of(tfm->__crt_alg,
				struct mtk_alg_template, alg.skcipher.base);
	unsigned long int flags = tmpl->flags;
	struct crypto_aes_ctx aes;
	unsigned int keylen = len;
	u32 nonce = 0;
	int ret = 0;

	if (!key || !keylen)
		return -EINVAL;

	if (IS_RFC3686(flags)) {
		/* last 4 bytes of key are the nonce! */
		keylen -= CTR_RFC3686_NONCE_SIZE;
		memcpy(&nonce, key + keylen, CTR_RFC3686_NONCE_SIZE);
	}

	switch ((flags & MTK_ALG_MASK)) {
	case MTK_ALG_AES:
		ret = aes_expandkey(&aes, key, keylen);
		break;
	case MTK_ALG_DES:
		ret = verify_skcipher_des_key(ctfm, key);
		break;
	case MTK_ALG_3DES:
		if (keylen != DES3_EDE_KEY_SIZE) {
			ret = -EINVAL;
			break;
		}
		ret = verify_skcipher_des3_key(ctfm, key);
	}

	if (ret) {
		crypto_skcipher_set_flags(ctfm, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return ret;
	}

	mtk_ctx_saRecord(ctx, key, nonce, keylen, flags);

	if (ctx->fallback) {
		ret = crypto_sync_skcipher_setkey(ctx->fallback, key, len);
		if (ret)
			return ret;
	}

	return 0;
}


static int mtk_skcipher_crypt(struct skcipher_request *req)
{
	struct mtk_cipher_reqctx *rctx = skcipher_request_ctx(req);
	struct crypto_async_request *base = &req->base;
	struct mtk_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct mtk_device *mtk = ctx->mtk;
	int ret;
	int DescriptorCountDone = MTK_RING_SIZE - 1;
	int DescriptorDoneTimeout = 15;
	int DescriptorPendingCount = 0;
	int commands = 0, results = 0;
	struct crypto_skcipher *skcipher = crypto_skcipher_reqtfm(req);
	u32 ivsize = crypto_skcipher_ivsize(skcipher);

	if (!req->cryptlen)
		return 0;

	rctx->textsize = req->cryptlen;
	rctx->authsize = 0;
	rctx->assoclen = 0;
	rctx->ivsize = ivsize;

	if ((req->cryptlen < NUM_AES_BYPASS) && (ctx->fallback)) {
		SYNC_SKCIPHER_REQUEST_ON_STACK(subreq, ctx->fallback);
		skcipher_request_set_sync_tfm(subreq, ctx->fallback);
		skcipher_request_set_callback(subreq, req->base.flags,
					NULL, NULL);
		skcipher_request_set_crypt(subreq, req->src, req->dst,
					req->cryptlen, req->iv);
		if (IS_ENCRYPT(rctx->flags))
			ret = crypto_skcipher_encrypt(subreq);
		else
			ret = crypto_skcipher_decrypt(subreq);

		skcipher_request_zero(subreq);
		return ret;
	}

	if (mtk->ring[0].requests > MTK_RING_BUSY)
		return -EAGAIN;

	ret = mtk_send_req(base, ctx, req->src, req->dst, req->iv,
				rctx, &commands, &results);

	if (ret) {
		base->complete(base, ret);
		return ret;
	}

	if (commands == 0)
		return 0;

	spin_lock_bh(&mtk->ring[0].lock);
	mtk->ring[0].requests += commands;

	if (!mtk->ring[0].busy) {
		DescriptorPendingCount = min_t(int, mtk->ring[0].requests, 32);
		writel(BIT(31) | (DescriptorCountDone & GENMASK(10, 0)) |
			(((DescriptorPendingCount - 1) & GENMASK(10, 0)) << 16) |
			((DescriptorDoneTimeout  & GENMASK(4, 0)) << 26),
			mtk->base + EIP93_REG_PE_RING_THRESH);
		mtk->ring[0].busy = true;
	}

	if (mtk->ring[0].requests > MTK_RING_BUSY) {
		ret = -EBUSY;
		rctx->flags |= MTK_BUSY;
	} else
		ret = -EINPROGRESS;

	spin_unlock_bh(&mtk->ring[0].lock);
	/* Writing new descriptor count starts DMA action */
	writel(commands, mtk->base + EIP93_REG_PE_CD_COUNT);

	return ret;
}

static int mtk_skcipher_encrypt(struct skcipher_request *req)
{
	struct mtk_cipher_reqctx *rctx = skcipher_request_ctx(req);
	struct crypto_async_request *base = &req->base;
	struct mtk_alg_template *tmpl = container_of(base->tfm->__crt_alg,
				struct mtk_alg_template, alg.skcipher.base);

	rctx->flags = tmpl->flags;
	rctx->flags |= MTK_ENCRYPT;

	return mtk_skcipher_crypt(req);
}

static int mtk_skcipher_decrypt(struct skcipher_request *req)
{
	struct mtk_cipher_reqctx *rctx = skcipher_request_ctx(req);
	struct crypto_async_request *base = &req->base;
	struct mtk_alg_template *tmpl = container_of(base->tfm->__crt_alg,
				struct mtk_alg_template, alg.skcipher.base);

	rctx->flags = tmpl->flags;
	rctx->flags |= MTK_DECRYPT;

	return mtk_skcipher_crypt(req);
}
/* Crypto aead API functions */
static int mtk_aead_cra_init(struct crypto_tfm *tfm)
{
	struct mtk_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	struct mtk_alg_template *tmpl = container_of(tfm->__crt_alg,
				struct mtk_alg_template, alg.aead.base);
	unsigned long int flags = tmpl->flags;
	char *alg_base;

	memset(ctx, 0, sizeof(*ctx));

	crypto_aead_set_reqsize(__crypto_aead_cast(tfm),
			sizeof(struct mtk_cipher_reqctx));

	ctx->mtk = tmpl->mtk;
	ctx->aead = true;
	ctx->base.handle_result = mtk_aead_handle_result;
	ctx->fallback = NULL;

	ctx->sa = kzalloc(sizeof(struct saRecord_s), GFP_KERNEL);
	if (!ctx->sa)
		printk("!! no sa memory\n");

	/* software workaround for now */
	if (IS_HASH_MD5(flags))
		alg_base = "md5";
	if (IS_HASH_SHA1(flags))
		alg_base = "sha1";
	if (IS_HASH_SHA224(flags))
		alg_base = "sha224";
	if (IS_HASH_SHA256(flags))
		alg_base = "sha256";

	ctx->shash = crypto_alloc_shash(alg_base, 0, CRYPTO_ALG_NEED_FALLBACK);

	if (IS_ERR(ctx->shash)) {
		dev_err(ctx->mtk->dev, "base driver %s could not be loaded.\n",
				alg_base);
		return PTR_ERR(ctx->shash);
	}

	return 0;
}

static void mtk_aead_cra_exit(struct crypto_tfm *tfm)
{
	struct mtk_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	if (ctx->shash)
		crypto_free_shash(ctx->shash);

	kfree(ctx->sa);
}

static int mtk_aead_setkey(struct crypto_aead *ctfm, const u8 *key,
			unsigned int keylen)
{
	struct crypto_tfm *tfm = crypto_aead_tfm(ctfm);
	struct mtk_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	struct mtk_alg_template *tmpl = container_of(tfm->__crt_alg,
				struct mtk_alg_template, alg.skcipher.base);
	unsigned long int flags = tmpl->flags;
	struct crypto_authenc_keys keys;
	int bs = crypto_shash_blocksize(ctx->shash);
	int ds = crypto_shash_digestsize(ctx->shash);
	u8 *ipad, *opad;
	unsigned int i, err;
	u32 nonce;

	SHASH_DESC_ON_STACK(shash, ctx->shash);

	if (crypto_authenc_extractkeys(&keys, key, keylen) != 0)
		goto badkey;

	if (IS_RFC3686(flags)) {
		if (keylen < CTR_RFC3686_NONCE_SIZE)
			return -EINVAL;

		keylen -= CTR_RFC3686_NONCE_SIZE;
		memcpy(&nonce, key + keylen, CTR_RFC3686_NONCE_SIZE);
	}

	if (keys.enckeylen > AES_MAX_KEY_SIZE)
		goto badkey;

	/* auth key
	 *
	 * EIP93 can only authenticate with hash of the key
	 * do software shash until EIP93 hash function complete.
	 */
	ipad = kcalloc(2, SHA512_BLOCK_SIZE, GFP_KERNEL);
 	if (!ipad)
 		return -ENOMEM;

	opad = ipad + SHA512_BLOCK_SIZE;

	shash->tfm = ctx->shash;

	if (keys.authkeylen > bs) {
		err = crypto_shash_digest(shash, keys.authkey,
					keys.authkeylen, ipad);
		if (err)
			return err;

		keys.authkeylen = ds;
	} else
		memcpy(ipad, keys.authkey, keys.authkeylen);

	memset(ipad + keys.authkeylen, 0, bs - keys.authkeylen);
	memcpy(opad, ipad, bs);

	for (i = 0; i < bs; i++) {
		ipad[i] ^= HMAC_IPAD_VALUE;
		opad[i] ^= HMAC_OPAD_VALUE;
	}

	err = crypto_shash_init(shash) ?:
				 crypto_shash_update(shash, ipad, bs) ?:
				 crypto_shash_export(shash, ipad) ?:
				 crypto_shash_init(shash) ?:
				 crypto_shash_update(shash, opad, bs) ?:
				 crypto_shash_export(shash, opad);

	if (err)
		return err;

	/* Encryption key */
	mtk_ctx_saRecord(ctx, keys.enckey, nonce, keys.enckeylen, flags);
	/* add auth key */
	memcpy(&ctx->sa->saIDigest, ipad, SHA256_DIGEST_SIZE);
	memcpy(&ctx->sa->saODigest, opad, SHA256_DIGEST_SIZE);

	kfree(ipad);
	return err;

badkey:
	crypto_aead_set_flags(ctfm, CRYPTO_TFM_RES_BAD_KEY_LEN);
	return -EINVAL;
}

static int mtk_aead_setauthsize(struct crypto_aead *ctfm,
				unsigned int authsize)
{
	struct crypto_tfm *tfm = crypto_aead_tfm(ctfm);
	struct mtk_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	/* might be needed for IPSec SHA1 (3 Words vs 5 Words)
	u32 maxauth = crypto_aead_maxauthsize(ctfm); */

	ctx->authsize = authsize;

	return 0;
}

static int mtk_aead_crypt(struct aead_request *req)
{
	struct mtk_cipher_reqctx *rctx = aead_request_ctx(req);
	struct crypto_async_request *base = &req->base;
	struct mtk_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct mtk_device *mtk = ctx->mtk;
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	u32 authsize = crypto_aead_authsize(aead);
	u32 ivsize = crypto_aead_ivsize(aead);
	int ret;
	int DescriptorCountDone = MTK_RING_SIZE - 1;
	int DescriptorDoneTimeout = 15;
	int DescriptorPendingCount = 0;
	int commands = 0, results = 0;

	rctx->textsize = req->cryptlen;
	rctx->assoclen = req->assoclen;
	rctx->authsize = authsize;
	rctx->ivsize = ivsize;

	if IS_DECRYPT(rctx->flags)
		rctx->textsize -= authsize;

	if (!rctx->textsize)
		return 0;

	if (mtk->ring[0].requests > MTK_RING_BUSY)
		return -EAGAIN;

	ret = mtk_send_req(base, ctx, req->src, req->dst, req->iv,
				rctx, &commands, &results);

	if (ret) {
		base->complete(base, ret);
		return ret;
	}

	if (commands == 0)
		return 0;

	spin_lock_bh(&mtk->ring[0].lock);
	mtk->ring[0].requests += commands;

	if (!mtk->ring[0].busy) {
		DescriptorPendingCount = mtk->ring[0].requests;
		writel(BIT(31) | (DescriptorCountDone & GENMASK(10, 0)) |
			(((DescriptorPendingCount - 1) & GENMASK(10, 0)) << 16) |
			((DescriptorDoneTimeout  & GENMASK(4, 0)) << 26),
			mtk->base + EIP93_REG_PE_RING_THRESH);
		mtk->ring[0].busy = true;
	}
	if (mtk->ring[0].requests > MTK_RING_BUSY) {
		ret = -EBUSY;
		rctx->flags |= MTK_BUSY;
	} else
		ret = -EINPROGRESS;

	spin_unlock_bh(&mtk->ring[0].lock);

	/* Writing new descriptor count starts DMA action */
	writel(commands, mtk->base + EIP93_REG_PE_CD_COUNT);

	return ret;
}

static int mtk_aead_encrypt(struct aead_request *req)
{
	struct mtk_cipher_reqctx *rctx = aead_request_ctx(req);
	struct crypto_async_request *base = &req->base;
	struct mtk_alg_template *tmpl = container_of(base->tfm->__crt_alg,
				struct mtk_alg_template, alg.aead.base);

	rctx->flags = tmpl->flags;
	rctx->flags |= MTK_ENCRYPT;

	return mtk_aead_crypt(req);
}

static int mtk_aead_decrypt(struct aead_request *req)
{
	struct mtk_cipher_reqctx *rctx = aead_request_ctx(req);
	struct crypto_async_request *base = &req->base;
	struct mtk_alg_template *tmpl = container_of(base->tfm->__crt_alg,
				struct mtk_alg_template, alg.aead.base);

	rctx->flags = tmpl->flags;
	rctx->flags |= MTK_DECRYPT;

	return mtk_aead_crypt(req);
}

/* Available algorithms in this module */

struct mtk_alg_template mtk_alg_ecb_des = {
	.type = MTK_ALG_TYPE_SKCIPHER,
	.flags = MTK_MODE_ECB | MTK_ALG_DES,
	.alg.skcipher = {
		.setkey = mtk_skcipher_setkey,
		.encrypt = mtk_skcipher_encrypt,
		.decrypt = mtk_skcipher_decrypt,
		.min_keysize = DES_KEY_SIZE,
		.max_keysize = DES_KEY_SIZE,
		.ivsize	= 0,
		.base = {
			.cra_name = "ecb(des)",
			.cra_driver_name = "ebc(des-eip93)",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_skcipher_cra_init,
			.cra_exit = mtk_skcipher_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_cbc_des = {
	.type = MTK_ALG_TYPE_SKCIPHER,
	.flags = MTK_MODE_CBC | MTK_ALG_DES,
	.alg.skcipher = {
		.setkey = mtk_skcipher_setkey,
		.encrypt = mtk_skcipher_encrypt,
		.decrypt = mtk_skcipher_decrypt,
		.min_keysize = DES_KEY_SIZE,
		.max_keysize = DES_KEY_SIZE,
		.ivsize	= DES_BLOCK_SIZE,
		.base = {
			.cra_name = "cbc(des)",
			.cra_driver_name = "cbc(des-eip93)",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_skcipher_cra_init,
			.cra_exit = mtk_skcipher_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_ecb_des3_ede = {
	.type = MTK_ALG_TYPE_SKCIPHER,
	.flags = MTK_MODE_ECB | MTK_ALG_3DES,
	.alg.skcipher = {
		.setkey = mtk_skcipher_setkey,
		.encrypt = mtk_skcipher_encrypt,
		.decrypt = mtk_skcipher_decrypt,
		.min_keysize = DES3_EDE_KEY_SIZE,
		.max_keysize = DES3_EDE_KEY_SIZE,
		.ivsize	= 0,
		.base = {
			.cra_name = "ecb(des3_ede)",
			.cra_driver_name = "ecb(des3_ede-eip93)",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_skcipher_cra_init,
			.cra_exit = mtk_skcipher_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_cbc_des3_ede = {
	.type = MTK_ALG_TYPE_SKCIPHER,
	.flags = MTK_MODE_CBC | MTK_ALG_3DES,
	.alg.skcipher = {
		.setkey = mtk_skcipher_setkey,
		.encrypt = mtk_skcipher_encrypt,
		.decrypt = mtk_skcipher_decrypt,
		.min_keysize = DES3_EDE_KEY_SIZE,
		.max_keysize = DES3_EDE_KEY_SIZE,
		.ivsize	= DES3_EDE_BLOCK_SIZE,
		.base = {
			.cra_name = "cbc(des3_ede)",
			.cra_driver_name = "cbc(des3_ede-eip93)",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_skcipher_cra_init,
			.cra_exit = mtk_skcipher_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_ecb_aes = {
	.type = MTK_ALG_TYPE_SKCIPHER,
	.flags = MTK_MODE_ECB | MTK_ALG_AES,
	.alg.skcipher = {
		.setkey = mtk_skcipher_setkey,
		.encrypt = mtk_skcipher_encrypt,
		.decrypt = mtk_skcipher_decrypt,
		.min_keysize = AES_MIN_KEY_SIZE,
		.max_keysize = AES_MAX_KEY_SIZE,
		.ivsize	= 0,
		.base = {
			.cra_name = "ecb(aes)",
			.cra_driver_name = "ecb(aes-eip93)",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0xf,
			.cra_init = mtk_skcipher_cra_init,
			.cra_exit = mtk_skcipher_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_cbc_aes = {
	.type = MTK_ALG_TYPE_SKCIPHER,
	.flags = MTK_MODE_CBC | MTK_ALG_AES,
	.alg.skcipher = {
		.setkey = mtk_skcipher_setkey,
		.encrypt = mtk_skcipher_encrypt,
		.decrypt = mtk_skcipher_decrypt,
		.min_keysize = AES_MIN_KEY_SIZE,
		.max_keysize = AES_MAX_KEY_SIZE,
		.ivsize	= AES_BLOCK_SIZE,
		.base = {
			.cra_name = "cbc(aes)",
			.cra_driver_name = "cbc(aes-eip93)",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0xf,
			.cra_init = mtk_skcipher_cra_init,
			.cra_exit = mtk_skcipher_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_ctr_aes = {
	.type = MTK_ALG_TYPE_SKCIPHER,
	.flags = MTK_MODE_CTR | MTK_ALG_AES,
	.alg.skcipher = {
		.setkey = mtk_skcipher_setkey,
		.encrypt = mtk_skcipher_encrypt,
		.decrypt = mtk_skcipher_decrypt,
		.min_keysize = AES_MIN_KEY_SIZE,
		.max_keysize = AES_MAX_KEY_SIZE,
		.ivsize	= AES_BLOCK_SIZE,
		.base = {
			.cra_name = "ctr(aes)",
			.cra_driver_name = "ctr(aes-eip93)",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0xf,
			.cra_init = mtk_skcipher_cra_init,
			.cra_exit = mtk_skcipher_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_rfc3686_aes = {
	.type = MTK_ALG_TYPE_SKCIPHER,
	.flags = MTK_MODE_CTR | MTK_MODE_RFC3686 | MTK_ALG_AES,
	.alg.skcipher = {
		.setkey = mtk_skcipher_setkey,
		.encrypt = mtk_skcipher_encrypt,
		.decrypt = mtk_skcipher_decrypt,
		.min_keysize = AES_MIN_KEY_SIZE + CTR_RFC3686_NONCE_SIZE,
		.max_keysize = AES_MAX_KEY_SIZE + CTR_RFC3686_NONCE_SIZE,
		.ivsize	= CTR_RFC3686_IV_SIZE,
		.base = {
			.cra_name = "rfc3686(ctr(aes))",
			.cra_driver_name = "rfc3686(ctr(aes-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0xf,
			.cra_init = mtk_skcipher_cra_init,
			.cra_exit = mtk_skcipher_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

/* Available authenc algorithms in this module */

struct mtk_alg_template mtk_alg_authenc_hmac_md5_cbc_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_MD5 | MTK_MODE_CBC | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= AES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = MD5_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(md5),cbc(aes))",
			.cra_driver_name =
				"authenc(hmac(md5-eip93), cbc(aes-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha1_cbc_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA1 | MTK_MODE_CBC | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= AES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA1_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha1),cbc(aes))",
			.cra_driver_name =
				"authenc(hmac(sha1-eip93),cbc(aes-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha224_cbc_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA224 | MTK_MODE_CBC | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= AES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA224_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha224),cbc(aes))",
			.cra_driver_name =
				"authenc(hmac(sha224-eip93),cbc(aes-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha256_cbc_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA256 | MTK_MODE_CBC | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= AES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA256_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha256),cbc(aes))",
			.cra_driver_name =
				"authenc(hmac(sha256-eip93),cbc(aes-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_md5_ctr_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_MD5 | MTK_MODE_CTR | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= AES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = MD5_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(md5),ctr(aes))",
			.cra_driver_name =
				"authenc(hmac(md5-eip93),ctr(aes-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha1_ctr_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA1 | MTK_MODE_CTR | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= AES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA1_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha1),ctr(aes))",
			.cra_driver_name =
				"authenc(hmac(sha1-eip93),ctr(aes-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha224_ctr_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA224 | MTK_MODE_CTR | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= AES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA224_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha224),ctr(aes))",
			.cra_driver_name =
				"authenc(hmac(sh224-eip93),ctr(aes-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha256_ctr_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA256 | MTK_MODE_CTR | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= AES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA256_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha256),ctr(aes))",
			.cra_driver_name =
				"authenc(hmac(sha256-eip93),ctr(aes-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_md5_rfc3686_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_MD5 |
			MTK_MODE_CTR | MTK_MODE_RFC3686 | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= CTR_RFC3686_IV_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = MD5_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(md5),rfc3686(ctr(aes)))",
			.cra_driver_name =
			"authenc(hmac(md5-eip93),rfc3686(ctr(aes-eip93)))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha1_rfc3686_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA1 |
			MTK_MODE_CTR | MTK_MODE_RFC3686 | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= CTR_RFC3686_IV_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA1_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha1),rfc3686(ctr(aes)))",
			.cra_driver_name =
			"authenc(hmac(sha1-eip93),rfc3686(ctr(aes-eip93)))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha224_rfc3686_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA224 |
			MTK_MODE_CTR | MTK_MODE_RFC3686 | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= CTR_RFC3686_IV_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA224_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha224),rfc3686(ctr(aes)))",
			.cra_driver_name =
			"authenc(hmac(sha224-eip93),rfc3686(ctr(aes-eip93)))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha256_rfc3686_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA256 |
			MTK_MODE_CTR | MTK_MODE_RFC3686 | MTK_ALG_AES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= CTR_RFC3686_IV_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA256_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha256),rfc3686(ctr(aes)))",
			.cra_driver_name =
			"authenc(hmac(sha256-eip93),rfc3686(ctr(aes-eip93)))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_md5_cbc_des = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_MD5 | MTK_MODE_CBC | MTK_ALG_DES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= DES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = MD5_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(md5),cbc(des))",
			.cra_driver_name =
				"authenc(hmac(md5-eip93),cbc(des-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha1_cbc_des = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA1 | MTK_MODE_CBC | MTK_ALG_DES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= DES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA1_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha1),cbc(des))",
			.cra_driver_name =
				"authenc(hmac(sha1-eip93),cbc(des-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha224_cbc_des = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA224 | MTK_MODE_CBC | MTK_ALG_DES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= DES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA224_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha224),cbc(des))",
			.cra_driver_name =
				"authenc(hmac(sha224-eip93),cbc(des-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha256_cbc_des = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA256 | MTK_MODE_CBC | MTK_ALG_DES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= DES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA256_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha256),cbc(des))",
			.cra_driver_name =
				"authenc(hmac(sha256-eip93),cbc(des-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_md5_cbc_des3_ede = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_MD5 | MTK_MODE_CBC | MTK_ALG_3DES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= DES3_EDE_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = MD5_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(md5),cbc(des3_ede))",
			.cra_driver_name =
				"authenc(hmac(md5-eip93),cbc(des3_ede-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0x0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha1_cbc_des3_ede = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA1 | MTK_MODE_CBC | MTK_ALG_3DES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= DES3_EDE_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA1_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha1),cbc(des3_ede))",
			.cra_driver_name =
				"authenc(hmac(sha1-eip93),cbc(des3_ede-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0x0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha224_cbc_des3_ede = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA224 | MTK_MODE_CBC | MTK_ALG_3DES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= DES3_EDE_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA224_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha224),cbc(des3_ede))",
			.cra_driver_name =
			"authenc(hmac(sha224-eip93),cbc(des3_ede-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0x0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha256_cbc_des3_ede = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA256 | MTK_MODE_CBC | MTK_ALG_3DES,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= DES3_EDE_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA256_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha256),cbc(des3_ede))",
			.cra_driver_name =
			"authenc(hmac(sha256-eip93),cbc(des3_ede-eip93))",
			.cra_priority = MTK_CRA_PRIORITY,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0x0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};
/* Single pass IPSEC ESP descriptor */
struct mtk_alg_template mtk_alg_authenc_hmac_md5_ecb_null = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_MD5,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= NULL_IV_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = MD5_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(md5),ecb(cipher_null))",
			.cra_driver_name = "eip93-authenc-hmac-md5-"
						"ecb-cipher-null",
			.cra_priority = 3000,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = NULL_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0x0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha1_ecb_null = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA1,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= NULL_IV_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA1_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha1),ecb(cipher_null))",
			.cra_driver_name = "eip93-authenc-hmac-sha1-"
						"ecb-cipher-null",
			.cra_priority = 3000,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = NULL_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0x0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha224_ecb_null = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA224,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= NULL_IV_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA224_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha224),ecb(cipher_null))",
			.cra_driver_name = "eip93-authenc-hmac-sha224-"
						"ecb-cipher-null",
			.cra_priority = 300,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = NULL_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0x0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_authenc_hmac_sha256_ecb_null = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA256,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= NULL_IV_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA256_DIGEST_SIZE,
		.base = {
			.cra_name = "authenc(hmac(sha256),ecb(cipher_null))",
			.cra_driver_name = "eip93-authenc-hmac-sha256-"
						"ecb-cipher-null",
			.cra_priority = 3000,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = NULL_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0x0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};

struct mtk_alg_template mtk_alg_echainiv_authenc_hmac_sha256_cbc_aes = {
	.type = MTK_ALG_TYPE_AEAD,
	.flags = MTK_HASH_HMAC | MTK_HASH_SHA256 | MTK_MODE_CBC |
			MTK_ALG_AES | MTK_GENIV,
	.alg.aead = {
		.setkey = mtk_aead_setkey,
		.encrypt = mtk_aead_encrypt,
		.decrypt = mtk_aead_decrypt,
		.ivsize	= AES_BLOCK_SIZE,
		.setauthsize = mtk_aead_setauthsize,
		.maxauthsize = SHA256_DIGEST_SIZE,
		.base = {
			.cra_name = "echainiv(authenc(hmac(sha256),cbc(aes)))",
			.cra_driver_name = "eip93-echainiv-authenc-hmac-sha256-cbc-aes",
			.cra_priority = 3000,
			.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_KERN_DRIVER_ONLY,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct mtk_cipher_ctx),
			.cra_alignmask = 0,
			.cra_init = mtk_aead_cra_init,
			.cra_exit = mtk_aead_cra_exit,
			.cra_module = THIS_MODULE,
		},
	},
};
