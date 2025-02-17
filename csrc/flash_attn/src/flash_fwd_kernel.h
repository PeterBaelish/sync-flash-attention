/******************************************************************************
 * Copyright (c) 2023, Tri Dao.
 ******************************************************************************/

#pragma once

#include <cmath>
#include <cute/algorithm/copy.hpp>
#include <cute/algorithm/gemm.hpp>

#include <cutlass/cutlass.h>
#include <cutlass/array.h>
#include <cutlass/numeric_types.h>
#include <cutlass/numeric_conversion.h>
#include <chrono>
#include "block_info.h"
#include "kernel_traits.h"
#include "utils.h"
#include "softmax.h"
#include "philox.cuh"

namespace flash {

using namespace cute;

////////////////////////////////////////////////////////////////////////////////////////////////////

template <int MMA_M,
          class... Args,
          class TiledMMA>
CUTE_HOST_DEVICE
auto
make_tiled_copy_A_warpcontiguousM(Copy_Atom<Args...> const& copy_atom,
                                 TiledMMA           const& tiled_mma) {
    using TileShape_MNK = typename TiledMMA::TiledShape_MNK;
    using AtomShape_MNK = typename TiledMMA::AtomShape_MNK;
    constexpr int AtomShape_M = decltype(size<0>(AtomShape_MNK{}))::value;
    constexpr int kNWarps = decltype(size<0>(TileShape_MNK{}))::value / AtomShape_M;
    constexpr int MMAStride_M = MMA_M * AtomShape_M;
    auto t = make_tile(Layout<Shape<Int<AtomShape_M>, Int<kNWarps>>,
                              Stride<_1, Int<MMAStride_M>> >{},
                       make_layout(size<2>(TileShape_MNK{})));
    // if (cute::thread0()) {printf("make_tiled_copy_A_warpcontiguousM "); print(t); printf("\n");  }
    return make_tiled_copy_impl(copy_atom, tiled_mma.get_layoutA_TV(), t);
}

//////////////////////////////////////////////////////////////////////////////////////////////////

template <int MMA_M,
          class... Args,
          class TiledMMA>
CUTE_HOST_DEVICE
auto
make_tiled_copy_C_warpcontiguousM(Copy_Atom<Args...> const& copy_atom,
                                 TiledMMA           const& tiled_mma) {
    using TileShape_MNK = typename TiledMMA::TiledShape_MNK;
    using AtomShape_MNK = typename TiledMMA::AtomShape_MNK;
    constexpr int AtomShape_M = decltype(size<0>(AtomShape_MNK{}))::value;
    constexpr int kNWarps = decltype(size<0>(TileShape_MNK{}))::value / AtomShape_M;
    constexpr int MMAStride_M = MMA_M * AtomShape_M;
    auto t = make_tile(Layout<Shape<Int<AtomShape_M>, Int<kNWarps>>,
                              Stride<_1, Int<MMAStride_M>> >{},
                       // TODO: Shouldn't this be size<1>?
                       make_layout(size<2>(TileShape_MNK{})));
    // if (cute::thread0()) {printf("make_tiled_copy_C_warpcontiguousM "); print(t); printf("\n");  }
    return make_tiled_copy_impl(copy_atom, tiled_mma.get_layoutC_TV(), t);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

template<bool Is_first, bool Check_inf=false, typename Tensor0, typename Tensor1, typename Tensor2>
inline __device__ void softmax_rescale_o(Tensor0 &scores, Tensor1 &scores_max, Tensor1 &scores_sum,
                                         Tensor2 &acc_o, float softmax_scale_log2) { 
    if (Is_first) {
        flash::template reduce_max</*zero_init=*/true>(scores, scores_max);
        flash::scale_apply_exp2(scores, scores_max, softmax_scale_log2);
        flash::reduce_sum(scores, scores_sum);
    } else {
        Tensor scores_max_prev = make_fragment_like(scores_max);
        cute::copy(scores_max, scores_max_prev);
        flash::template reduce_max</*zero_init=*/false>(scores, scores_max);
        // Reshape acc_o from (MMA=4, MMA_M, MMA_K) to (nrow=(2, MMA_M), ncol=(2, MMA_K))
        Tensor acc_o_rowcol = make_tensor(acc_o.data(), flash::convert_layout_acc_rowcol(acc_o.layout()));
        #pragma unroll
        for (int mi = 0; mi < size(scores_max); ++mi) {
            float scores_max_cur = !Check_inf
                ? scores_max(mi)
                : (scores_max(mi) == -INFINITY ? 0.0f : scores_max(mi));
            float scores_scale = exp2f((scores_max_prev(mi) - scores_max_cur) * softmax_scale_log2);
            scores_sum(mi) *= scores_scale;
            #pragma unroll
            for (int ni = 0; ni < size<1>(acc_o_rowcol); ++ni) { acc_o_rowcol(mi, ni) *= scores_scale; }
        }
        flash::scale_apply_exp2(scores, scores_max, softmax_scale_log2);
        Tensor scores_sum_cur = make_fragment_like(scores_sum);
        flash::reduce_sum(scores, scores_sum_cur);
        #pragma unroll
        for (int mi = 0; mi < size(scores_sum); ++mi) { scores_sum(mi) += scores_sum_cur(mi); }
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////

//  Bae: merge two fragments of O, stores at acc_o_2. Also merge scores_sum and scores_max,
//       scores_sum stores at scores_sum_1, scores_max stores at scores_max_1

template<bool Check_inf=false, typename Tensor1, typename Tensor2>
inline __device__ void softmax_merge_o(Tensor1 &scores_max_1, Tensor1 &scores_sum_1,
                                       Tensor1 &scores_max_2, Tensor1 &scores_sum_2,
                                       Tensor2 &acc_o_1, Tensor2 &acc_o_2, 
                                       float softmax_scale_log2) {
    //int tidx = threadIdx.x;
    //int block_id = blockIdx.x + blockIdx.y * gridDim.x + gridDim.x * gridDim.y * blockIdx.z;
    
    Tensor scores_max = make_fragment_like(scores_max_1);
    // Reshape acc_o from (MMA=4, MMA_M, MMA_K) to (nrow=(2, MMA_M), ncol=(2, MMA_K))
    Tensor acc_o_1_rowcol = make_tensor(acc_o_1.data(), flash::convert_layout_acc_rowcol(acc_o_1.layout()));
    Tensor acc_o_2_rowcol = make_tensor(acc_o_2.data(), flash::convert_layout_acc_rowcol(acc_o_2.layout()));
    #pragma unroll
    for (int mi = 0; mi < size(scores_max_1); ++mi) {
        // k = l(2)/l(1) * e^(m(2)-m(1))
        scores_max(mi) = scores_max_2(mi) > scores_max_1(mi) ? scores_max_2(mi) : scores_max_1(mi);
        float scores_scale = (scores_sum_2(mi) / scores_sum_1(mi))
                             * exp2f((scores_max_2(mi) - scores_max_1(mi)) * softmax_scale_log2);
        scores_scale = 1.0 / (1.0 + scores_scale);
        /*if(block_id == 0 && tidx == 66) {
            printf("scores_sum_2(mi) = %f, scores_sum_1(mi) = %f, scores_max_2(mi) = %f, scores_max_1(mi) = %f, scores_scale = %f\n", scores_sum_2(mi), scores_sum_1(mi), 
                scores_max_2(mi), scores_max_1(mi), scores_scale);
            printf("k = %f\n", (scores_sum_2(mi) / scores_sum_1(mi)) * exp2f((scores_max_2(mi) - scores_max_1(mi)) * softmax_scale_log2));
            printf("s-m = %f\n", exp2f((scores_max_2(mi) - scores_max_1(mi)) * softmax_scale_log2));
        }*/
        #pragma unroll
        for (int ni = 0; ni < size<1>(acc_o_1_rowcol); ++ni) {
            /*if(block_id == 0 && tidx == 66)
                printf("acc_o_1_rowcol(mi, ni) = %f, acc_o_2_rowcol(mi, ni) = %f\n", acc_o_1_rowcol(mi, ni), acc_o_2_rowcol(mi, ni));*/
            acc_o_2_rowcol(mi, ni) = acc_o_1_rowcol(mi, ni) * scores_scale + acc_o_2_rowcol(mi, ni) * (1.0 - scores_scale);
            /*if(block_id == 0 && tidx == 66)
                printf("merged acc_o_2_rowcol(mi, ni) = %f\n", acc_o_2_rowcol(mi, ni));*/
        }
    }
    //We also need to compute and store l,m for LSE
    #pragma unroll
    for (int mi = 0; mi < size(scores_sum_1); ++mi) {
        scores_sum_1(mi) = scores_sum_1(mi) * exp2f((scores_max_1(mi) - scores_max(mi)) * softmax_scale_log2) 
                            + scores_sum_2(mi) * exp2f((scores_max_2(mi) - scores_max(mi)) * softmax_scale_log2); 
        scores_max_1(mi) = scores_max(mi);
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename Engine0, typename Layout0, typename Engine1, typename Layout1, typename TiledCopy>
inline __device__ void write_softmax_to_gmem(
    Tensor<Engine0, Layout0> const &tOrP, Tensor<Engine1, Layout1> &tPgP, TiledCopy gmem_tiled_copy_P
) {
    // Reshape tOrP from (8, MMA_M, MMA_N) to (8, MMA_M * MMA_N)
    Layout l = tOrP.layout();
    Tensor tPrP = make_tensor(tOrP.data(), make_layout(get<0>(l), make_layout(get<1>(l), get<2>(l))));
    CUTE_STATIC_ASSERT_V(size<2>(tPgP) == _1{});
    CUTE_STATIC_ASSERT_V(size<1>(tPrP) == size<1>(tPgP));
    #pragma unroll
    for (int mi = 0; mi < size<1>(tPrP); ++mi) {
        cute::copy(gmem_tiled_copy_P, tPrP(_, mi), tPgP(_, mi, 0));
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename Kernel_traits, bool Is_dropout, bool Is_causal, bool Is_even_N, bool Is_even_K, bool Return_softmax, typename Params>
inline __device__ void compute_attn_1rowblock(const Params &params, const int bidb, const int bidh, const int m_block) {

    using Element = typename Kernel_traits::Element;
    using ElementAccum = typename Kernel_traits::ElementAccum;
    using index_t = typename Kernel_traits::index_t;

    // Shared memory.
    extern __shared__ char smem_[];

    // The thread index.
    const int tidx = threadIdx.x;
    // The global block index.
    const int block_id = blockIdx.x + blockIdx.y * gridDim.x + gridDim.x * gridDim.y * blockIdx.z;

    constexpr int kBlockM = Kernel_traits::kBlockM;
    constexpr int kBlockN = Kernel_traits::kBlockN;
    constexpr int kHeadDim = Kernel_traits::kHeadDim;
    constexpr int kNWarps = Kernel_traits::kNWarps;
    constexpr int MMA_M = kBlockM / decltype(size<0>(typename Kernel_traits::TiledMma::TiledShape_MNK{}))::value;

    const BlockInfo</*Varlen=*/!Is_even_N> binfo(params, bidb);
    if (m_block * kBlockM >= binfo.actual_seqlen_q || binfo.actual_seqlen_k == 0) return;

    int n_block_max = cute::ceil_div(binfo.actual_seqlen_k, kBlockN);
    if (Is_causal) {
        n_block_max = std::min(n_block_max, cute::ceil_div((m_block + 1) * kBlockM, kBlockN));
        // if (threadIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0) {
        //     printf("m_block = %d, n_block_max = %d\n", m_block, n_block_max);
        // }
    }

    // We iterate over the blocks in reverse order. This is because the last block is the only one
    // that needs masking when we read K and V from global memory. Moreover, iterating in reverse
    // might save us 1 register (we just need n_block instead of both n_block and n_block_max).

    const index_t row_offset_q = binfo.q_offset(params.q_batch_stride, params.q_row_stride, bidb)
        + m_block * kBlockM * params.q_row_stride + bidh * params.q_head_stride;
    // We move K and V to the last block.
    const index_t row_offset_k = binfo.k_offset(params.k_batch_stride, params.k_row_stride, bidb)
        + (n_block_max - 1) * kBlockN * params.k_row_stride + (bidh / params.h_h_k_ratio) * params.k_head_stride;
    const index_t row_offset_v = binfo.k_offset(params.v_batch_stride, params.v_row_stride, bidb)
        + (n_block_max - 1) * kBlockN * params.v_row_stride + (bidh / params.h_h_k_ratio) * params.v_head_stride;
    const index_t row_offset_p = ((bidb * params.h + bidh) * params.seqlen_q_rounded
        + m_block * kBlockM) * params.seqlen_k_rounded + (n_block_max - 1) * kBlockN;

    Tensor gQ = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.q_ptr) + row_offset_q),
                            Shape<Int<kBlockM>, Int<kHeadDim>>{},
                            make_stride(params.q_row_stride, _1{}));
    Tensor gK = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.k_ptr) + row_offset_k),
                            Shape<Int<kBlockN>, Int<kHeadDim>>{},
                            make_stride(params.k_row_stride, _1{}));
    Tensor gV = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.v_ptr) + row_offset_v),
                            Shape<Int<kBlockN>, Int<kHeadDim>>{},
                            make_stride(params.v_row_stride, _1{}));
    Tensor gP = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.p_ptr) + row_offset_p),
                            Shape<Int<kBlockM>, Int<kBlockN>>{},
                            make_stride(params.seqlen_k_rounded, _1{}));

    Tensor sQ = make_tensor(make_smem_ptr(reinterpret_cast<Element *>(smem_)),
                            typename Kernel_traits::SmemLayoutQ{});
    // Careful we're using the same smem for sQ and sK | sV if Share_Q_K_smem;
    Tensor sK = make_tensor(sQ.data() + (Kernel_traits::Share_Q_K_smem ? 0 : size(sQ)),
                            typename Kernel_traits::SmemLayoutKV{});
    Tensor sV = make_tensor(sK.data() + size(sK), typename Kernel_traits::SmemLayoutKV{});
    Tensor sVt = make_tensor(sV.data(), typename Kernel_traits::SmemLayoutVtransposed{});
    Tensor sVtNoSwizzle = make_tensor(sV.data(), typename Kernel_traits::SmemLayoutVtransposedNoSwizzle{});

    typename Kernel_traits::GmemTiledCopyQKV gmem_tiled_copy_QKV;
    auto gmem_thr_copy_QKV = gmem_tiled_copy_QKV.get_thread_slice(tidx);
    typename Kernel_traits::GmemTiledCopyP gmem_tiled_copy_P;
    auto gmem_thr_copy_P = gmem_tiled_copy_P.get_thread_slice(tidx);

    Tensor tQgQ = gmem_thr_copy_QKV.partition_S(gQ);
    Tensor tQsQ = gmem_thr_copy_QKV.partition_D(sQ);
    Tensor tKgK = gmem_thr_copy_QKV.partition_S(gK);  // (KCPY, KCPY_N, KCPY_K)
    Tensor tKsK = gmem_thr_copy_QKV.partition_D(sK);
    Tensor tVgV = gmem_thr_copy_QKV.partition_S(gV);  // (VCPY, VCPY_N, VCPY_K)
    Tensor tVsV = gmem_thr_copy_QKV.partition_D(sV);
    Tensor tPgP = gmem_thr_copy_P.partition_D(gP);

    typename Kernel_traits::TiledMma tiled_mma;
    auto thr_mma = tiled_mma.get_thread_slice(tidx);
    Tensor tSrQ  = thr_mma.partition_fragment_A(sQ);                           // (MMA,MMA_M,MMA_K)
    Tensor tSrK  = thr_mma.partition_fragment_B(sK);                           // (MMA,MMA_N,MMA_K)
    Tensor tOrVt  = thr_mma.partition_fragment_B(sVtNoSwizzle);                // (MMA, MMA_K,MMA_N)

    Tensor acc_o = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kHeadDim>>{});  // MMA, MMA_M, MMA_K

    //
    // Copy Atom retiling
    //

    auto smem_tiled_copy_Q = make_tiled_copy_A(typename Kernel_traits::SmemCopyAtom{}, tiled_mma);
    auto smem_thr_copy_Q = smem_tiled_copy_Q.get_thread_slice(tidx);
    // auto smem_thr_copy_Q = make_tiled_copy_A_warpcontiguousM<MMA_M>(typename Kernel_traits::SmemCopyAtom{}, tiled_mma).get_thread_slice(tidx);
    // if (cute::thread0()) {smem_thr_copy_Q.print_all();}
    Tensor tSsQ = smem_thr_copy_Q.partition_S(sQ);
    // if (cute::thread0()) {print(tSsQ.layout()); printf("\n");}

    auto smem_tiled_copy_K = make_tiled_copy_B(typename Kernel_traits::SmemCopyAtom{}, tiled_mma);
    auto smem_thr_copy_K = smem_tiled_copy_K.get_thread_slice(tidx);
    Tensor tSsK = smem_thr_copy_K.partition_S(sK);

    auto smem_tiled_copy_V = make_tiled_copy_B(typename Kernel_traits::SmemCopyAtomTransposed{}, tiled_mma);
    auto smem_thr_copy_V = smem_tiled_copy_V.get_thread_slice(tidx);
    Tensor tOsVt = smem_thr_copy_V.partition_S(sVt);

    // TODO: this might need to change if we change the mma instruction in SM70
    Tensor scores_max = make_tensor<ElementAccum>(Shape<Int<2 * size<1>(acc_o)>>{});
    Tensor scores_sum = make_fragment_like(scores_max);

    //
    // PREDICATES
    //

    // // Allocate predicate tensors for m and n
    // Tensor tQpQ = make_tensor<bool>(make_shape(size<1>(tQsQ), size<2>(tQsQ)), Stride<_1,_0>{});
    // Tensor tKVpKV = make_tensor<bool>(make_shape(size<1>(tKsK), size<2>(tKsK)), Stride<_1,_0>{});

    // Construct identity layout for sQ and sK
    Tensor cQ = make_identity_tensor(make_shape(size<0>(sQ), size<1>(sQ)));    // (BLK_M,BLK_K) -> (blk_m,blk_k)
    Tensor cKV = make_identity_tensor(make_shape(size<0>(sK), size<1>(sK)));    // (BLK_N,BLK_K) -> (blk_n,blk_k)
    // Tensor tScQ = thr_mma.partition_A(cQ);                           // (MMA,MMA_M,MMA_K)
    // if (cute::thread0()) {
    //     print(tScQ.layout()); printf("\n");
    //     for (int i = 0; i < size(tScQ); ++i) {
    //         printf("%d ", get<0>(tScQ(i)));
    //     }
    //     printf("\n");
    //     for (int i = 0; i < size(tScQ); ++i) {
    //         printf("%d ", get<1>(tScQ(i)));
    //     }
    //     printf("\n");
    // }

    // Repeat the partitioning with identity layouts
    Tensor tQcQ = gmem_thr_copy_QKV.partition_S(cQ);       // (ACPY,ACPY_M,ACPY_K) -> (blk_m,blk_k)
    Tensor tKVcKV = gmem_thr_copy_QKV.partition_S(cKV);   // (BCPY,BCPY_N,BCPY_K) -> (blk_n,blk_k)

    // Allocate predicate tensors for k
    Tensor tQpQ = make_tensor<bool>(make_shape(size<2>(tQsQ)));
    Tensor tKVpKV = make_tensor<bool>(make_shape(size<2>(tKsK)));

    // Set predicates for k bounds
    if (!Is_even_K) {
        #pragma unroll
        for (int k = 0; k < size(tQpQ); ++k) { tQpQ(k) = get<1>(tQcQ(0, 0, k)) < params.d; }
        #pragma unroll
        for (int k = 0; k < size(tKVpKV); ++k) { tKVpKV(k) = get<1>(tKVcKV(0, 0, k)) < params.d; }
    }

    // Prologue

    Tensor tQrQ = make_fragment_like(tQgQ);
    // We don't need to clear the sQ smem tiles since we'll only write out the valid outputs
    flash::copy</*Is_even_MN=*/false, Is_even_K>(gmem_tiled_copy_QKV, tQgQ, tQsQ, tQcQ, tQpQ,
                                                 binfo.actual_seqlen_q - m_block * kBlockM);
    if (Kernel_traits::Is_Q_in_regs) { cute::cp_async_fence(); }

    // // Copy rmem to smem
    // // copy(tQrQ, tQsQ);
    // flash::cp_async_wait<0>();
    // __syncthreads();
    // // if (cute::thread(1, 0)) { print(tQsQ); }
    // // Tensor sQNoSwizzle = make_tensor(make_smem_ptr(reinterpret_cast<Element *>(smem_)), typename Kernel_traits::SmemLayoutQNoSwizzle{});
    // // if (cute::thread0()) { print(sQNoSwizzle); }

    if (Kernel_traits::Share_Q_K_smem) {
        flash::cp_async_wait<0>();
        __syncthreads();
        Tensor tSrQ_copy_view = smem_thr_copy_Q.retile_D(tSrQ);
        CUTE_STATIC_ASSERT_V(size<1>(tSsQ) == size<1>(tSrQ_copy_view));            // M
        cute::copy(smem_tiled_copy_Q, tSsQ, tSrQ_copy_view);
        __syncthreads();
    }

    int n_block = n_block_max - 1;
    // We don't need to clear the sK smem tiles since we'll mask out the scores anyway.
    flash::copy<Is_even_N, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKVcKV, tKVpKV,
                                      binfo.actual_seqlen_k - n_block * kBlockN);
    cute::cp_async_fence();
    // if (threadIdx.x == 0 && blockIdx.y == 0 && blockIdx.z < 2) { print(tKgK); }
    // __syncthreads();

    if (Kernel_traits::Is_Q_in_regs && !Kernel_traits::Share_Q_K_smem) {
        flash::cp_async_wait<1>();
        __syncthreads();
        Tensor tSrQ_copy_view = smem_thr_copy_Q.retile_D(tSrQ);
        CUTE_STATIC_ASSERT_V(size<1>(tSsQ) == size<1>(tSrQ_copy_view));            // M
        cute::copy(smem_tiled_copy_Q, tSsQ, tSrQ_copy_view);
    }

    auto seeds = at::cuda::philox::unpack(params.philox_args);
    unsigned long long seed = std::get<0>(seeds);
    unsigned long long offset = std::get<1>(seeds) + (bidb * params.h + bidh) * 32 + tidx % 32;

    // Save seed and offset for backward.
    if (block_id == 0 && tidx == 0) {
        params.rng_state[0] = seed;
        params.rng_state[1] = std::get<1>(seeds);
    }

    clear(acc_o);

    // For performance reason, we separate out two kinds of iterations:
    // those that need masking on S, and those that don't.
    // We need masking on S for the very last block when K and V has length not multiple of kBlockN.
    // We also need masking on S if it's causal, for the last ceil_div(kBlockM, kBlockN) blocks.
    // We will have at least 1 "masking" iteration.

    constexpr int n_masking_steps = Is_causal ? cute::ceil_div(kBlockM, kBlockN) : 1;
    #pragma unroll
    for (int masking_step = 0; masking_step < n_masking_steps; ++masking_step, --n_block) {
        Tensor acc_s = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kBlockN>>{});  // (MMA=4, MMA_M, MMA_N)
        clear(acc_s);
        flash::cp_async_wait<0>();
        __syncthreads();

        // Advance gV
        if (masking_step > 0) {
            tVgV.data() = tVgV.data() + (-int(kBlockN * params.v_row_stride));
            flash::copy</*Is_even_MN=*/true, Is_even_K>(gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV);
        } else {
            // Clear the smem tiles to account for predicated off loads
            flash::copy<Is_even_N, Is_even_K, /*Clear_OOB_MN=*/true>(
                gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV, binfo.actual_seqlen_k - n_block * kBlockN
            );
        }
        cute::cp_async_fence();

        flash::gemm</*A_in_regs=*/Kernel_traits::Is_Q_in_regs>(
            acc_s, tSrQ, tSrK, tSsQ, tSsK, tiled_mma, smem_tiled_copy_Q, smem_tiled_copy_K,
            smem_thr_copy_Q, smem_thr_copy_K
        );
        // if (cute::thread0()) { print(acc_s); }

        // Reshape acc_s from (MMA=4, MMA_M, MMA_N) to (nrow=(2, MMA_M), ncol=(2, MMA_N))
        Tensor scores = make_tensor(acc_s.data(), flash::convert_layout_acc_rowcol(acc_s.layout()));
        // if (cute::thread0()) { print(scores); }
        // We don't put the masking before the matmul S = Q K^T because we don't clear sK
        // for rows outside actual_seqlen_k. So those rows could have Inf / NaN, and the matmul
        // can produce Inf / NaN.
        if (!Is_causal) {
            if (!Is_even_N) { flash::apply_mask(scores, binfo.actual_seqlen_k - n_block * kBlockN); }
        } else {
            // Tensor caccS = make_identity_tensor(Shape<Int<kBlockM>, Int<kBlockN>>{});    // (BLK_M,BLK_N) -> (blk_m,blk_n)
            // Tensor taccScS = thr_mma.partition_C(caccS);                           // (MMA,MMA_M,MMA_N)
            // static_assert(decltype(size<0>(taccScS))::value == 4);
            // // Convert to ((2, 2), MMA_M, MMA_N) then take only the row indices.
            // Tensor idx_row = logical_divide(taccScS, Shape<_2>{})(make_coord(0, _), _, 0);
            // Tensor idx_rowcol = make_tensor(taccScS.data(), flash::convert_layout_acc_rowcol(taccScS.layout()));
            // flash::apply_mask_causal_w_idx(scores, idx_rowcol, n_block * kBlockN, binfo.actual_seqlen_k,
            //                               m_block * kBlockM);
            // Idk why it's get<1> and not get<0> of the stride.
            // if (cute::thread0()) { print(idx_row.layout()); print(stride<1>(idx_row)); printf("stride = %d \n", get<1>(stride<1>(idx_row))); }
            // I can't get the stride from idx_row
            flash::apply_mask_causal(scores, n_block * kBlockN, binfo.actual_seqlen_k,
                                     // m_block * kBlockM + get<0>(idx_row(0)),
                                     m_block * kBlockM + (tidx / 32) * 16 + (tidx % 32) / 4,
                                     kNWarps * 16);
                                     // m_block * kBlockM + (tidx / 32) * 16, kNWarps * 16);
                                     // m_block * kBlockM + (tidx / 32) * (kBlockM / kNWarps), 16);
        }

        flash::cp_async_wait<0>();
        __syncthreads();
        if (n_block > 0) {
            // Advance gK
            tKgK.data() = tKgK.data() + (-int(kBlockN * params.k_row_stride));
            flash::copy</*Is_even_MN=*/true, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKVcKV, tKVpKV);
            // This cp_async_fence needs to be in the if block, otherwise the synchronization
            // isn't right and we get race conditions.
            cute::cp_async_fence();
        }

        // TODO: when we have key_padding_mask we'll need to Check_inf
        masking_step == 0
            ? softmax_rescale_o</*Is_first=*/true,  /*Check_inf=*/Is_causal>(scores, scores_max, scores_sum, acc_o, params.scale_softmax_log2)
            : softmax_rescale_o</*Is_first=*/false, /*Check_inf=*/Is_causal>(scores, scores_max, scores_sum, acc_o, params.scale_softmax_log2);

        // Convert scores from fp32 to fp16/bf16
        Tensor rP = flash::convert_type<Element>(scores);
        // Reshape rP from (nrow=(2, MMA_M), ncol=(2, MMA_N)) to ((2, 2, 2), MMA_M, MMA_N / 2)
        // if using m16n8k16 or ((2, 2, 1), MMA_M, MMA_N) if using m16n8k8.
        Tensor tOrP = make_tensor(rP.data(), flash::convert_layout_rowcol_Aregs<Kernel_traits::TiledMma>(rP.layout()));
        uint32_t block_row_idx = m_block * (kBlockM / 16) + tidx / 32;
        uint32_t block_col_idx = n_block * (kBlockN / 32);
        if (Return_softmax) {
            Tensor tOrP_copy = make_fragment_like(tOrP);
            cute::copy(tOrP, tOrP_copy);
            flash::apply_dropout</*encode_dropout_in_sign_bit=*/true>(
                tOrP_copy, params.p_dropout_in_uint8_t, seed, offset,
                block_row_idx, block_col_idx, kNWarps
            );
            flash::write_softmax_to_gmem(tOrP_copy, tPgP, gmem_tiled_copy_P);
            tPgP.data() = tPgP.data() + (-kBlockN);
        }
        if (Is_dropout) {
            flash::apply_dropout(tOrP, params.p_dropout_in_uint8_t, seed, offset,
                                 block_row_idx, block_col_idx, kNWarps);
        }
        // if (cute::thread0()) { print(tOrP); }

        flash::gemm_A_in_regs(acc_o, tOrP, tOrVt, tOsVt, tiled_mma, smem_tiled_copy_V, smem_thr_copy_V);
        // if (cute::thread0()) { print(scores); }

        // This check is at the end of the loop since we always have at least 1 iteration
        if (n_masking_steps > 1 && n_block <= 0) {
            --n_block;
            break;
        }
    }

    // These are the iterations where we don't need masking on S
    for (; n_block >= 0; --n_block) {
        Tensor acc_s = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kBlockN>>{});  // (MMA=4, MMA_M, MMA_N)
        clear(acc_s);
        flash::cp_async_wait<0>();
        __syncthreads();
        // Advance gV
        tVgV.data() = tVgV.data() + (-int(kBlockN * params.v_row_stride));
        flash::copy</*Is_even_MN=*/true, Is_even_K>(gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV);
        cute::cp_async_fence();

        flash::gemm</*A_in_regs=*/Kernel_traits::Is_Q_in_regs>(
            acc_s, tSrQ, tSrK, tSsQ, tSsK, tiled_mma, smem_tiled_copy_Q, smem_tiled_copy_K,
            smem_thr_copy_Q, smem_thr_copy_K
        );

        flash::cp_async_wait<0>();
        __syncthreads();
        if (n_block > 0) {
            // Advance gK
            tKgK.data() = tKgK.data() + (-int(kBlockN * params.k_row_stride));
            flash::copy</*Is_even_MN=*/true, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKVcKV, tKVpKV);
            // This cp_async_fence needs to be in the if block, otherwise the synchronization
            // isn't right and we get race conditions.
            cute::cp_async_fence();
        }

        // Reshape acc_s from (MMA=4, MMA_M, MMA_N) to (nrow=(2, MMA_M), ncol=(2, MMA_N))
        Tensor scores = make_tensor(acc_s.data(), flash::convert_layout_acc_rowcol(acc_s.layout()));
        softmax_rescale_o</*Is_first=*/false>(scores, scores_max, scores_sum, acc_o, params.scale_softmax_log2);

        Tensor rP = flash::convert_type<Element>(scores);
        // Reshape rP from (nrow=(2, MMA_M), ncol=(2, MMA_N)) to ((2, 2, 2), MMA_M, MMA_N / 2)
        // if using m16n8k16 or ((2, 2, 1), MMA_M, MMA_N) if using m16n8k8.
        Tensor tOrP = make_tensor(rP.data(), flash::convert_layout_rowcol_Aregs<Kernel_traits::TiledMma>(rP.layout()));
        uint32_t block_row_idx = m_block * (kBlockM / 16) + tidx / 32;
        uint32_t block_col_idx = n_block * (kBlockN / 32);
        if (Return_softmax) {
            Tensor tOrP_copy = make_fragment_like(tOrP);
            cute::copy(tOrP, tOrP_copy);
            flash::apply_dropout</*encode_dropout_in_sign_bit=*/true>(
                tOrP_copy, params.p_dropout_in_uint8_t, seed, offset,
                block_row_idx, block_col_idx, kNWarps
            );
            flash::write_softmax_to_gmem(tOrP_copy, tPgP, gmem_tiled_copy_P);
            tPgP.data() = tPgP.data() + (-kBlockN);
        }
        if (Is_dropout) {
            flash::apply_dropout(tOrP, params.p_dropout_in_uint8_t, seed, offset,
                                 block_row_idx, block_col_idx, kNWarps);
        }

        flash::gemm_A_in_regs(acc_o, tOrP, tOrVt, tOsVt, tiled_mma, smem_tiled_copy_V, smem_thr_copy_V);
    }

    // Epilogue

    // Reshape acc_o from (MMA=4, MMA_M, MMA_K) to (nrow=(2, MMA_M), ncol=(2, MMA_K))
    Tensor acc_o_rowcol = make_tensor(acc_o.data(), flash::convert_layout_acc_rowcol(acc_o.layout()));
    Tensor lse = make_fragment_like(scores_sum);
    #pragma unroll
    for (int mi = 0; mi < size<0>(acc_o_rowcol); ++mi) {
        float sum = scores_sum(mi);
        float inv_sum = (sum == 0.f || sum != sum) ? 1.f : 1.f / sum;
        lse(mi) = (sum == 0.f || sum != sum) ? INFINITY : scores_max(mi) * params.scale_softmax + __logf(sum);
        float scale = !Is_dropout ? inv_sum : inv_sum * params.rp_dropout;
        #pragma unroll
        for (int ni = 0; ni < size<1>(acc_o_rowcol); ++ni) { acc_o_rowcol(mi, ni) *= scale; }
    }

    // if (cute::thread0()) { print(acc_o_rowcol); }

    // Convert acc_o from fp32 to fp16/bf16
    Tensor rO = flash::convert_type<Element>(acc_o);
    Tensor sO = make_tensor(sQ.data(), typename Kernel_traits::SmemLayoutO{});    // (SMEM_M,SMEM_N)
    // Partition sO to match the accumulator partitioning
    auto smem_tiled_copy_O = make_tiled_copy_C(typename Kernel_traits::SmemCopyAtomO{}, tiled_mma);
    auto smem_thr_copy_O = smem_tiled_copy_O.get_thread_slice(tidx);
    // auto smem_thr_copy_O = make_tiled_copy_C_warpcontiguousM<MMA_M>(typename Kernel_traits::SmemCopyAtomO{}, tiled_mma).get_thread_slice(tidx);
    Tensor taccOrO = smem_thr_copy_O.retile_S(rO);        // ((Atom,AtomNum), MMA_M, MMA_N)
    Tensor taccOsO = smem_thr_copy_O.partition_D(sO);     // ((Atom,AtomNum),PIPE_M,PIPE_N)

    // sO has the same size as sQ, so we don't need to sync here.
    if (Kernel_traits::Share_Q_K_smem) { __syncthreads(); }

    cute::copy(smem_tiled_copy_O, taccOrO, taccOsO);

    const index_t row_offset_o = binfo.q_offset(params.o_batch_stride, params.o_row_stride, bidb)
        + m_block * kBlockM * params.o_row_stride + bidh * params.o_head_stride;
    const index_t row_offset_lse = (bidb * params.h + bidh) * params.seqlen_q + m_block * kBlockM;
    Tensor gO = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.o_ptr) + row_offset_o),
                            Shape<Int<kBlockM>, Int<kHeadDim>>{},
                            make_stride(params.o_row_stride, _1{}));
    Tensor gLSE = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.softmax_lse_ptr) + row_offset_lse),
                              Shape<Int<kBlockM>>{}, Stride<_1>{});

    typename Kernel_traits::GmemTiledCopyO gmem_tiled_copy_O;
    auto gmem_thr_copy_O = gmem_tiled_copy_O.get_thread_slice(tidx);
    Tensor tOsO = gmem_thr_copy_O.partition_S(sO);        // ((Atom,AtomNum),ATOM_M,ATOM_N)
    Tensor tOgO = gmem_thr_copy_O.partition_D(gO);

    __syncthreads();

    Tensor tOrO = make_tensor<Element>(shape(tOgO));
    cute::copy(gmem_tiled_copy_O, tOsO, tOrO);

    Tensor caccO = make_identity_tensor(Shape<Int<kBlockM>, Int<kHeadDim>>{});    // (BLK_M,BLK_K) -> (blk_m,blk_k)
    Tensor taccOcO = thr_mma.partition_C(caccO);                           // (MMA,MMA_M,MMA_K)
    static_assert(decltype(size<0>(taccOcO))::value == 4);
    // Convert to ((2, 2), MMA_M, MMA_K) then take only the row indices.
    Tensor taccOcO_row = logical_divide(taccOcO, Shape<_2>{})(make_coord(0, _), _, 0);
    CUTE_STATIC_ASSERT_V(size(lse) == size(taccOcO_row));                     // MMA_M
    if (get<1>(taccOcO_row(0)) == 0) {
        #pragma unroll
        for (int mi = 0; mi < size(lse); ++mi) {
            const int row = get<0>(taccOcO_row(mi));
            if (row < binfo.actual_seqlen_q - m_block * kBlockM) { gLSE(row) = lse(mi); }
        }
    }

    // Construct identity layout for sO
    Tensor cO = make_identity_tensor(make_shape(size<0>(sO), size<1>(sO)));    // (BLK_M,BLK_K) -> (blk_m,blk_k)
    // Repeat the partitioning with identity layouts
    Tensor tOcO = gmem_thr_copy_O.partition_D(cO);                           // (ACPY,ACPY_M,ACPY_K) -> (blk_m,blk_k)
    Tensor tOpO = make_tensor<bool>(make_shape(size<2>(tOgO)));
    if (!Is_even_K) {
        #pragma unroll
        for (int k = 0; k < size(tOpO); ++k) { tOpO(k) = get<1>(tOcO(0, 0, k)) < params.d; }
    }
    // Clear_OOB_K must be false since we don't want to write zeros to gmem
    flash::copy</*Is_even_MN=*/false, Is_even_K, /*Clear_OOB_MN=*/false, /*Clear_OOB_K=*/false>(
        gmem_tiled_copy_O, tOrO, tOgO, tOcO, tOpO, binfo.actual_seqlen_q - m_block * kBlockM
    );
    // uint64_t start = clock64();
    // uint64_t end = start + (uint64_t)(1000000000);
    // while(clock64() < end) {
    //         float dummy = 0.0;
    //         for(int i=0;i<1000;i++){
    //                 dummy += tanf(dummy) + logf(dummy);
    //         }
    // }
}

__device__ inline uint64_t GlobalTimer64(void) {
  // Due to a bug in CUDA's 64-bit globaltimer, the lower 32 bits can wrap
  // around after the upper bits have already been read. Work around this by
  // reading the high bits a second time. Use the second value to detect a
  // rollover, and set the lower bits of the 64-bit "timer reading" to 0, which
  // would be valid, it's passed over during the duration of the reading. If no
  // rollover occurred, just return the initial reading.
  volatile uint64_t first_reading;
  volatile uint32_t second_reading;
  uint32_t high_bits_first;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(first_reading));
  high_bits_first = first_reading >> 32;
  asm volatile("mov.u32 %0, %%globaltimer_hi;" : "=r"(second_reading));
  if (high_bits_first == second_reading) {
    return first_reading;
  }
  // Return the value with the updated high bits, but the low bits set to 0.
  return ((uint64_t) second_reading) << 32;
}

// Returns the ID of the SM this is executed on.
static __device__ __inline__ uint32_t GetSMID(void) {
  uint32_t to_return;
  asm volatile("mov.u32 %0, %%smid;" : "=r"(to_return));
  return to_return;
}


__device__ int CompleteMask[32][32][1024];
//__device__ int CompleteMask;

template<typename Kernel_traits, bool Is_dropout, bool Is_causal, bool Is_even_N, bool Is_even_K, bool Return_softmax, typename Params>
inline __device__ void compute_attn_1rowblock_causal(const Params &params, const int bidb, const int bidh, const int m_block) {

    /*
    
    TODO:

    if(m_block < N/2)
        copy ptr(m_block) to shd mem
        Do m_block row first
        store ptr(m_block) to glb mem
        copy ptr(N-m_block) to shd mem
        Do (N-m_block) row fragment 1 to d/2-f(m_block)
    else
        copy ptr(m_block) to shd mem
        Do (m_block) row fragment  f(m_block)-d/2 to f(m_block) 
        store ptr(m_block) to glb mem
    
    syncthreads

    if(m_block < N/2)
        copy ptr(N-m_block) row fragment d/2-f(m_block) to d-f(m_block) to shd mem
        plus (N-m_block) row fragment d/2-f(m_block) to d-f(m_block)  and (N-m_block) row fragment 1 to d/2-f(m_block)
        copy ptr(N-m_block) row to glb mem

    */

    //if (cute::thread0()) { printf("fence -7\n"); }
    
    //uint64_t start_time = GlobalTimer64();
    //uint32_t sm_id = GetSMID();


    using Element = typename Kernel_traits::Element;
    using ElementAccum = typename Kernel_traits::ElementAccum;
    using index_t = typename Kernel_traits::index_t;

    // Shared memory.
    extern __shared__ char smem_[];

    // The thread index.
    const int tidx = threadIdx.x;
    // The global block index.
    const int block_id = blockIdx.x + blockIdx.y * gridDim.x + gridDim.x * gridDim.y * blockIdx.z;

    constexpr int kBlockM = Kernel_traits::kBlockM;
    constexpr int kBlockN = Kernel_traits::kBlockN;
    constexpr int kHeadDim = Kernel_traits::kHeadDim;
    constexpr int kNWarps = Kernel_traits::kNWarps;
    constexpr int MMA_M = kBlockM / decltype(size<0>(typename Kernel_traits::TiledMma::TiledShape_MNK{}))::value;

    const BlockInfo<!Is_even_N> binfo(params, bidb);

    /**/
    if (m_block + 1 > (((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) / 2) + 1 && threadIdx.x == 0) {
        atomicAnd(&CompleteMask[bidh][bidb][blockIdx.x], 0);
    }
    

    if (m_block * kBlockM >= binfo.actual_seqlen_q || binfo.actual_seqlen_k == 0) return;
    
    int n_block_max = cute::ceil_div(binfo.actual_seqlen_k, kBlockN);
    
    n_block_max = std::min(n_block_max, cute::ceil_div((m_block + 1) * kBlockM, kBlockN)); //causal
        // if (threadIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0) {
        //     printf("m_block = %d, n_block_max = %d\n", m_block, n_block_max);
        // }

    // We iterate over the blocks in reverse order. This is because the last block is the only one
    // that needs masking when we read K and V from global memory. Moreover, iterating in reverse
    // might save us 1 register (we just need n_block instead of both n_block and n_block_max).

    const index_t row_offset_q = binfo.q_offset(params.q_batch_stride, params.q_row_stride, bidb)
        + m_block * kBlockM * params.q_row_stride + bidh * params.q_head_stride;
    // We move K and V to the last block.
    const index_t row_offset_k = binfo.k_offset(params.k_batch_stride, params.k_row_stride, bidb)
        + 0 * kBlockN * params.k_row_stride + (bidh / params.h_h_k_ratio) * params.k_head_stride;
    const index_t row_offset_v = binfo.k_offset(params.v_batch_stride, params.v_row_stride, bidb)
        + 0 * kBlockN * params.v_row_stride + (bidh / params.h_h_k_ratio) * params.v_head_stride;
    const index_t row_offset_p = ((bidb * params.h + bidh) * params.seqlen_q_rounded
        + m_block * kBlockM) * params.seqlen_k_rounded + 0 * kBlockN;

    Tensor gQ = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.q_ptr) + row_offset_q),
                            Shape<Int<kBlockM>, Int<kHeadDim>>{},
                            make_stride(params.q_row_stride, _1{}));
    Tensor gK = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.k_ptr) + row_offset_k),
                            Shape<Int<kBlockN>, Int<kHeadDim>>{},
                            make_stride(params.k_row_stride, _1{}));
    Tensor gV = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.v_ptr) + row_offset_v),
                            Shape<Int<kBlockN>, Int<kHeadDim>>{},
                            make_stride(params.v_row_stride, _1{}));
    Tensor gP = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.p_ptr) + row_offset_p),
                            Shape<Int<kBlockM>, Int<kBlockN>>{},
                            make_stride(params.seqlen_k_rounded, _1{}));
    
    Tensor sQ = make_tensor(make_smem_ptr(reinterpret_cast<Element *>(smem_)),
                            typename Kernel_traits::SmemLayoutQ{});
    // Careful we're using the same smem for sQ and sK | sV if Share_Q_K_smem;
    Tensor sK = make_tensor(sQ.data() + (Kernel_traits::Share_Q_K_smem ? 0 : size(sQ)),
                            typename Kernel_traits::SmemLayoutKV{});
    Tensor sV = make_tensor(sK.data() + size(sK), typename Kernel_traits::SmemLayoutKV{});
    Tensor sVt = make_tensor(sV.data(), typename Kernel_traits::SmemLayoutVtransposed{});
    Tensor sVtNoSwizzle = make_tensor(sV.data(), typename Kernel_traits::SmemLayoutVtransposedNoSwizzle{});

    //if (cute::thread0()) { printf("fence -6.5\n"); }

    typename Kernel_traits::GmemTiledCopyQKV gmem_tiled_copy_QKV;
    auto gmem_thr_copy_QKV = gmem_tiled_copy_QKV.get_thread_slice(tidx);
    typename Kernel_traits::GmemTiledCopyP gmem_tiled_copy_P;
    auto gmem_thr_copy_P = gmem_tiled_copy_P.get_thread_slice(tidx);

    Tensor tQgQ = gmem_thr_copy_QKV.partition_S(gQ);
    Tensor tQsQ = gmem_thr_copy_QKV.partition_D(sQ);
    Tensor tKgK = gmem_thr_copy_QKV.partition_S(gK);  // (KCPY, KCPY_N, KCPY_K)
    Tensor tKsK = gmem_thr_copy_QKV.partition_D(sK);
    Tensor tVgV = gmem_thr_copy_QKV.partition_S(gV);  // (VCPY, VCPY_N, VCPY_K)
    Tensor tVsV = gmem_thr_copy_QKV.partition_D(sV);
    Tensor tPgP = gmem_thr_copy_P.partition_D(gP);

    typename Kernel_traits::TiledMma tiled_mma;
    auto thr_mma = tiled_mma.get_thread_slice(tidx);
    Tensor tSrQ  = thr_mma.partition_fragment_A(sQ);                           // (MMA,MMA_M,MMA_K)
    Tensor tSrK  = thr_mma.partition_fragment_B(sK);                           // (MMA,MMA_N,MMA_K)
    Tensor tOrVt  = thr_mma.partition_fragment_B(sVtNoSwizzle);                // (MMA, MMA_K,MMA_N)

    //Bae: acc_o size is (4, B_r/2, Headdim/2)
    Tensor acc_o = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kHeadDim>>{});  // MMA, MMA_M, MMA_K
    //Bae: acc_s size is (4, B_r/2, B_c/2)
    //Tensor acc_s = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kBlockN>>{});  // (MMA=4, MMA_M, MMA_N)
    
    //
    // Copy Atom retiling
    //

    auto smem_tiled_copy_Q = make_tiled_copy_A(typename Kernel_traits::SmemCopyAtom{}, tiled_mma);
    auto smem_thr_copy_Q = smem_tiled_copy_Q.get_thread_slice(tidx);
    // auto smem_thr_copy_Q = make_tiled_copy_A_warpcontiguousM<MMA_M>(typename Kernel_traits::SmemCopyAtom{}, tiled_mma).get_thread_slice(tidx);
    // if (cute::thread0()) {smem_thr_copy_Q.print_all();}
    Tensor tSsQ = smem_thr_copy_Q.partition_S(sQ);
    // if (cute::thread0()) {print(tSsQ.layout()); printf("\n");}

    auto smem_tiled_copy_K = make_tiled_copy_B(typename Kernel_traits::SmemCopyAtom{}, tiled_mma);
    auto smem_thr_copy_K = smem_tiled_copy_K.get_thread_slice(tidx);
    Tensor tSsK = smem_thr_copy_K.partition_S(sK);

    auto smem_tiled_copy_V = make_tiled_copy_B(typename Kernel_traits::SmemCopyAtomTransposed{}, tiled_mma);
    auto smem_thr_copy_V = smem_tiled_copy_V.get_thread_slice(tidx);
    Tensor tOsVt = smem_thr_copy_V.partition_S(sVt);

    // TODO: this might need to change if we change the mma instruction in SM70

    //if (cute::thread0()) { printf("fence -6\n"); }

    //Bae: B_r
    Tensor scores_max = make_tensor<ElementAccum>(Shape<Int<2 * size<1>(acc_o)>>{}); 
    Tensor scores_sum = make_fragment_like(scores_max);

    //
    // PREDICATES
    //

    // // Allocate predicate tensors for m and n
    // Tensor tQpQ = make_tensor<bool>(make_shape(size<1>(tQsQ), size<2>(tQsQ)), Stride<_1,_0>{});
    // Tensor tKVpKV = make_tensor<bool>(make_shape(size<1>(tKsK), size<2>(tKsK)), Stride<_1,_0>{});

    // Construct identity layout for sQ and sK
    Tensor cQ = make_identity_tensor(make_shape(size<0>(sQ), size<1>(sQ)));    // (BLK_M,BLK_K) -> (blk_m,blk_k)
    Tensor cKV = make_identity_tensor(make_shape(size<0>(sK), size<1>(sK)));    // (BLK_N,BLK_K) -> (blk_n,blk_k)
    // Tensor tScQ = thr_mma.partition_A(cQ);                           // (MMA,MMA_M,MMA_K)
    // if (cute::thread0()) {
    //     print(tScQ.layout()); printf("\n");
    //     for (int i = 0; i < size(tScQ); ++i) {
    //         printf("%d ", get<0>(tScQ(i)));
    //     }
    //     printf("\n");
    //     for (int i = 0; i < size(tScQ); ++i) {
    //         printf("%d ", get<1>(tScQ(i)));
    //     }
    //     printf("\n");
    // }

    // Repeat the partitioning with identity layouts
    Tensor tQcQ = gmem_thr_copy_QKV.partition_S(cQ);       // (ACPY,ACPY_M,ACPY_K) -> (blk_m,blk_k)
    Tensor tKVcKV = gmem_thr_copy_QKV.partition_S(cKV);   // (BCPY,BCPY_N,BCPY_K) -> (blk_n,blk_k)

    // Allocate predicate tensors for k
    Tensor tQpQ = make_tensor<bool>(make_shape(size<2>(tQsQ)));
    Tensor tKVpKV = make_tensor<bool>(make_shape(size<2>(tKsK)));

    // Set predicates for k bounds
    if (!Is_even_K) {
        #pragma unroll
        for (int k = 0; k < size(tQpQ); ++k) { tQpQ(k) = get<1>(tQcQ(0, 0, k)) < params.d; }
        #pragma unroll
        for (int k = 0; k < size(tKVpKV); ++k) { tKVpKV(k) = get<1>(tKVcKV(0, 0, k)) < params.d; }
    }

    //if (cute::thread0()) { printf("fence -5\n"); }

    // Prologue

    Tensor tQrQ = make_fragment_like(tQgQ);
    // We don't need to clear the sQ smem tiles since we'll only write out the valid outputs
    flash::copy<false, Is_even_K>(gmem_tiled_copy_QKV, tQgQ, tQsQ, tQcQ, tQpQ,
                                                 binfo.actual_seqlen_q - m_block * kBlockM);
    if (Kernel_traits::Is_Q_in_regs) { cute::cp_async_fence(); }

    // // Copy rmem to smem
    // // copy(tQrQ, tQsQ);
    // flash::cp_async_wait<0>();
    // __syncthreads();
    // // if (cute::thread(1, 0)) { print(tQsQ); }
    // // Tensor sQNoSwizzle = make_tensor(make_smem_ptr(reinterpret_cast<Element *>(smem_)), typename Kernel_traits::SmemLayoutQNoSwizzle{});
    // // if (cute::thread0()) { print(sQNoSwizzle); }

    if (Kernel_traits::Share_Q_K_smem) {
        flash::cp_async_wait<0>();
        __syncthreads();
        Tensor tSrQ_copy_view = smem_thr_copy_Q.retile_D(tSrQ);
        CUTE_STATIC_ASSERT_V(size<1>(tSsQ) == size<1>(tSrQ_copy_view));            // M
        cute::copy(smem_tiled_copy_Q, tSsQ, tSrQ_copy_view);
        __syncthreads();
    }

    int n_block = 0;
    int dst = n_block_max - 1;

    //Bae: If m_block > floor(N/2), we only compute ceil(d/2) blocks
    if(m_block + 1 > (((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) / 2) + 1){ 
        dst = cute::ceil_div(kBlockM, kBlockN) * ((cute::ceil_div(binfo.actual_seqlen_q, kBlockM) / 2) + 1) - 1; 
    }  
    
    // We don't need to clear the sK smem tiles since we'll mask out the scores anyway.
    flash::copy<Is_even_N, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKVcKV, tKVpKV,
                                      binfo.actual_seqlen_k - n_block * kBlockN);
    cute::cp_async_fence();
    // if (threadIdx.x == 0 && blockIdx.y == 0 && blockIdx.z < 2) { print(tKgK); }
    // __syncthreads();

    if (Kernel_traits::Is_Q_in_regs && !Kernel_traits::Share_Q_K_smem) {
        flash::cp_async_wait<1>();
        __syncthreads();
        Tensor tSrQ_copy_view = smem_thr_copy_Q.retile_D(tSrQ);
        CUTE_STATIC_ASSERT_V(size<1>(tSsQ) == size<1>(tSrQ_copy_view));            // M
        cute::copy(smem_tiled_copy_Q, tSsQ, tSrQ_copy_view);
    }

    auto seeds = at::cuda::philox::unpack(params.philox_args);
    unsigned long long seed = std::get<0>(seeds);
    unsigned long long offset = std::get<1>(seeds) + (bidb * params.h + bidh) * 32 + tidx % 32;

    // Save seed and offset for backward.
    if (block_id == 0 && tidx == 0) {
        params.rng_state[0] = seed;
        params.rng_state[1] = std::get<1>(seeds);
    }

    clear(acc_o);

    //if (cute::thread0()) { printf("fence -4\n"); }

    // For performance reason, we separate out two kinds of iterations:
    // those that need masking on S, and those that don't.
    // We need masking on S for the very last block when K and V has length not multiple of kBlockN.
    // We also need masking on S if it's causal, for the last ceil_div(kBlockM, kBlockN) blocks.
    // We will have at least 1 "masking" iteration.

    //Bae: n_masking_steps controls the masking of very last block, n_block control the whole number of masking blocks

    int n_masking_steps = m_block + 1 > (((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) / 2) + 1 ? 0 : cute::ceil_div(kBlockM, kBlockN); //Bae: this is for the last causal block

    // These are the iterations where we don't need masking on S
    for (; n_block <= dst - n_masking_steps; ++n_block) {
        Tensor acc_s = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kBlockN>>{});  // (MMA=4, MMA_M, MMA_N)
        clear(acc_s);
        flash::cp_async_wait<0>();
        __syncthreads();

        // Advance gV
        if (n_block > 0) {
            tVgV.data() = tVgV.data() + (+int(kBlockN * params.v_row_stride));
            flash::copy<true, Is_even_K>(gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV);
        } else {
            // Clear the smem tiles to account for predicated off loads
            flash::copy<Is_even_N, Is_even_K, true>(
                gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV, binfo.actual_seqlen_k - n_block * kBlockN
            );
        }
        cute::cp_async_fence();

        flash::gemm<Kernel_traits::Is_Q_in_regs>(
            acc_s, tSrQ, tSrK, tSsQ, tSsK, tiled_mma, smem_tiled_copy_Q, smem_tiled_copy_K,
            smem_thr_copy_Q, smem_thr_copy_K
        );

        flash::cp_async_wait<0>();
        __syncthreads();
        if (!(n_masking_steps == 0 && n_block == dst)) {
            // Advance gK
            tKgK.data() = tKgK.data() + (+int(kBlockN * params.k_row_stride));
            flash::copy<true, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKVcKV, tKVpKV);
            // This cp_async_fence needs to be in the if block, otherwise the synchronization
            // isn't right and we get race conditions.
            cute::cp_async_fence();
        }

        // Reshape acc_s from (MMA=4, MMA_M, MMA_N) to (nrow=(2, MMA_M), ncol=(2, MMA_N))
        Tensor scores = make_tensor(acc_s.data(), flash::convert_layout_acc_rowcol(acc_s.layout()));
        
        n_block == 0
            ? softmax_rescale_o<true,  true>(scores, scores_max, scores_sum, acc_o, params.scale_softmax_log2)
            : softmax_rescale_o<false, true>(scores, scores_max, scores_sum, acc_o, params.scale_softmax_log2);

        Tensor rP = flash::convert_type<Element>(scores);
        // Reshape rP from (nrow=(2, MMA_M), ncol=(2, MMA_N)) to ((2, 2, 2), MMA_M, MMA_N / 2)
        // if using m16n8k16 or ((2, 2, 1), MMA_M, MMA_N) if using m16n8k8.
        Tensor tOrP = make_tensor(rP.data(), flash::convert_layout_rowcol_Aregs<Kernel_traits::TiledMma>(rP.layout()));
        uint32_t block_row_idx = m_block * (kBlockM / 16) + tidx / 32;
        uint32_t block_col_idx = n_block * (kBlockN / 32);
        if (Return_softmax) {
            Tensor tOrP_copy = make_fragment_like(tOrP);
            cute::copy(tOrP, tOrP_copy);
            flash::apply_dropout<true>(
                tOrP_copy, params.p_dropout_in_uint8_t, seed, offset,
                block_row_idx, block_col_idx, kNWarps
            );
            flash::write_softmax_to_gmem(tOrP_copy, tPgP, gmem_tiled_copy_P);
            tPgP.data() = tPgP.data() + (+kBlockN);
        }
        if (Is_dropout) {
            flash::apply_dropout(tOrP, params.p_dropout_in_uint8_t, seed, offset,
                                 block_row_idx, block_col_idx, kNWarps);
        }
        flash::gemm_A_in_regs(acc_o, tOrP, tOrVt, tOsVt, tiled_mma, smem_tiled_copy_V, smem_thr_copy_V);
    }

    #pragma unroll
    for (; n_block <= dst; ++n_block) {
        Tensor acc_s = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kBlockN>>{});
        clear(acc_s);
        flash::cp_async_wait<0>();
        __syncthreads();

        // Advance gV
        if (dst == 0) {
            flash::copy<Is_even_N, Is_even_K, true>(
                gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV, binfo.actual_seqlen_k - n_block * kBlockN
            );
        } else {
            tVgV.data() = tVgV.data() + (+int(kBlockN * params.v_row_stride));
            flash::copy<true, Is_even_K>(gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV);
        }
        cute::cp_async_fence();

        flash::gemm<Kernel_traits::Is_Q_in_regs>(
            acc_s, tSrQ, tSrK, tSsQ, tSsK, tiled_mma, smem_tiled_copy_Q, smem_tiled_copy_K,
            smem_thr_copy_Q, smem_thr_copy_K
        );
        // if (cute::thread0()) { print(acc_s); }

        // Reshape acc_s from (MMA=4, MMA_M, MMA_N) to (nrow=(2, MMA_M), ncol=(2, MMA_N))
        Tensor scores = make_tensor(acc_s.data(), flash::convert_layout_acc_rowcol(acc_s.layout()));
        // if (cute::thread0()) { print(scores); }
        // We don't put the masking before the matmul S = Q K^T because we don't clear sK
        // for rows outside actual_seqlen_k. So those rows could have Inf / NaN, and the matmul
        // can produce Inf / NaN.

            // Tensor caccS = make_identity_tensor(Shape<Int<kBlockM>, Int<kBlockN>>{});    // (BLK_M,BLK_N) -> (blk_m,blk_n)
            // Tensor taccScS = thr_mma.partition_C(caccS);                           // (MMA,MMA_M,MMA_N)
            // static_assert(decltype(size<0>(taccScS))::value == 4);
            // // Convert to ((2, 2), MMA_M, MMA_N) then take only the row indices.
            // Tensor idx_row = logical_divide(taccScS, Shape<_2>{})(make_coord(0, _), _, 0);
            // Tensor idx_rowcol = make_tensor(taccScS.data(), flash::convert_layout_acc_rowcol(taccScS.layout()));
            // flash::apply_mask_causal_w_idx(scores, idx_rowcol, n_block * kBlockN, binfo.actual_seqlen_k,
            //                               m_block * kBlockM);
            // Idk why it's get<1> and not get<0> of the stride.
            // if (cute::thread0()) { print(idx_row.layout()); print(stride<1>(idx_row)); printf("stride = %d \n", get<1>(stride<1>(idx_row))); }
            // I can't get the stride from idx_row
            flash::apply_mask_causal(scores, n_block * kBlockN, binfo.actual_seqlen_k,
                                     // m_block * kBlockM + get<0>(idx_row(0)),
                                     m_block * kBlockM + (tidx / 32) * 16 + (tidx % 32) / 4,
                                     kNWarps * 16);
                                     // m_block * kBlockM + (tidx / 32) * 16, kNWarps * 16);
                                     // m_block * kBlockM + (tidx / 32) * (kBlockM / kNWarps), 16);

        flash::cp_async_wait<0>();
        __syncthreads();
        if (n_block < dst) {
            // Advance gK
            tKgK.data() = tKgK.data() + (+int(kBlockN * params.k_row_stride));
            flash::copy<true, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKVcKV, tKVpKV);
            // This cp_async_fence needs to be in the if block, otherwise the synchronization
            // isn't right and we get race conditions.
            cute::cp_async_fence();
        }

        softmax_rescale_o<false, true>(scores, scores_max, scores_sum, acc_o, params.scale_softmax_log2);

        // Convert scores from fp32 to fp16/bf16
        Tensor rP = flash::convert_type<Element>(scores);
        // Reshape rP from (nrow=(2, MMA_M), ncol=(2, MMA_N)) to ((2, 2, 2), MMA_M, MMA_N / 2)
        // if using m16n8k16 or ((2, 2, 1), MMA_M, MMA_N) if using m16n8k8.
        Tensor tOrP = make_tensor(rP.data(), flash::convert_layout_rowcol_Aregs<Kernel_traits::TiledMma>(rP.layout()));
        uint32_t block_row_idx = m_block * (kBlockM / 16) + tidx / 32;
        uint32_t block_col_idx = n_block * (kBlockN / 32);
        if (Return_softmax) {
            Tensor tOrP_copy = make_fragment_like(tOrP);
            cute::copy(tOrP, tOrP_copy);
            flash::apply_dropout<true>(
                tOrP_copy, params.p_dropout_in_uint8_t, seed, offset,
                block_row_idx, block_col_idx, kNWarps
            );
            flash::write_softmax_to_gmem(tOrP_copy, tPgP, gmem_tiled_copy_P);
            tPgP.data() = tPgP.data() + (+kBlockN);
        }
        if (Is_dropout) {
            flash::apply_dropout(tOrP, params.p_dropout_in_uint8_t, seed, offset,
                                 block_row_idx, block_col_idx, kNWarps);
        }
        // if (cute::thread0()) { print(tOrP); }

        flash::gemm_A_in_regs(acc_o, tOrP, tOrVt, tOsVt, tiled_mma, smem_tiled_copy_V, smem_thr_copy_V);
        // if (cute::thread0()) { print(scores); }

        // This check is at the end of the loop since we always have at least 1 iteration
        /*if (n_masking_steps > 1 && n_block <= dst) {
            --n_block;
            break;
        }*/
    }

    //if (cute::thread0()) { printf("fence -3\n"); }
    
    //if (cute::thread0()) { printf("fence -2\n"); }
    /*__threadfence();
    atomicAnd(&CompleteMask, 0);
    if (tidx == 0) {
        while ((atomicOr(&CompleteMask, 1ULL << block_id)) != SollMask);
    }
    __syncthreads();
    if (m_block == ((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) - 1 && tidx == 66) 
    { 
        printf("fragment:\n");
        printf("scores_max:\n");
        print(scores_max);
        printf("scores_sum:\n");
        print(scores_sum);
        printf("acc_o:\n");
        print(acc_o);
    }*/
    // Epilogue

    // Reshape acc_o from (MMA=4, MMA_M, MMA_K) to (nrow=(2, MMA_M), ncol=(2, MMA_K))
    // Bae: That's (Br, d) !
    Tensor acc_o_rowcol = make_tensor(acc_o.data(), flash::convert_layout_acc_rowcol(acc_o.layout()));
    Tensor lse = make_fragment_like(scores_sum);
    #pragma unroll
    for (int mi = 0; mi < size<0>(acc_o_rowcol); ++mi) {
        float sum = scores_sum(mi);
        float inv_sum = (sum == 0.f || sum != sum) ? 1.f : 1.f / sum;
        lse(mi) = (sum == 0.f || sum != sum) ? INFINITY : scores_max(mi) * params.scale_softmax + __logf(sum);
        float scale = !Is_dropout ? inv_sum : inv_sum * params.rp_dropout;
        #pragma unroll
        for (int ni = 0; ni < size<1>(acc_o_rowcol); ++ni) { acc_o_rowcol(mi, ni) *= scale; }
        /*if (m_block == ((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) - 1 && tidx == 66) 
        { 
            printf("scale = %f\n", scale);
        }*/
    }

    // if (cute::thread0()) { print(acc_o_rowcol); }
    /*if (m_block == ((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) - 1 && tidx == 66) 
    { 
        printf("acc_o:\n");
        print(acc_o);
    }
    __threadfence();
    atomicAnd(&CompleteMask, 0);
    if (tidx == 0) {
        while ((atomicOr(&CompleteMask, 1ULL << block_id)) != SollMask);
    }
    __syncthreads();*/
    // Convert acc_o from fp32 to fp16/bf16
    Tensor rO = flash::convert_type<Element>(acc_o);
    //Bae: O in shared memory replace Q!!
    Tensor sO = make_tensor(sQ.data(), typename Kernel_traits::SmemLayoutO{});    // (SMEM_M,SMEM_N)
    // Partition sO to match the accumulator partitioning
    auto smem_tiled_copy_O = make_tiled_copy_C(typename Kernel_traits::SmemCopyAtomO{}, tiled_mma);
    auto smem_thr_copy_O = smem_tiled_copy_O.get_thread_slice(tidx);
    // auto smem_thr_copy_O = make_tiled_copy_C_warpcontiguousM<MMA_M>(typename Kernel_traits::SmemCopyAtomO{}, tiled_mma).get_thread_slice(tidx);
    Tensor taccOrO = smem_thr_copy_O.retile_S(rO);        // ((Atom,AtomNum), MMA_M, MMA_N)
    Tensor taccOsO = smem_thr_copy_O.partition_D(sO);     // ((Atom,AtomNum),PIPE_M,PIPE_N)

    // sO has the same size as sQ, so we don't need to sync here.
    if (Kernel_traits::Share_Q_K_smem) { __syncthreads(); }

    cute::copy(smem_tiled_copy_O, taccOrO, taccOsO);

    //if (cute::thread0()) { printf("fence -1\n"); }

    const index_t row_offset_o = binfo.q_offset(params.o_batch_stride, params.o_row_stride, bidb)
        + m_block * kBlockM * params.o_row_stride + bidh * params.o_head_stride;
    const index_t row_offset_lse = (bidb * params.h + bidh) * params.seqlen_q + m_block * kBlockM;
    Tensor gO = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.o_ptr) + row_offset_o),
                            Shape<Int<kBlockM>, Int<kHeadDim>>{},
                            make_stride(params.o_row_stride, _1{}));
    Tensor gLSE = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.softmax_lse_ptr) + row_offset_lse),
                            Shape<Int<kBlockM>>{}, Stride<_1>{});
    // Bae: scores_max, scores_sum should be stored at glb mem, they are familiar to Q,O, only difference is kheaddim=1
    const index_t row_offset_scores_max = (bidb * params.h + bidh) * params.seqlen_q + m_block * kBlockM;
    const index_t row_offset_scores_sum = (bidb * params.h + bidh) * params.seqlen_q + m_block * kBlockM;
    Tensor gscores_max = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.scores_max_ptr) +row_offset_scores_max),
                            Shape<Int<kBlockM>>{}, Stride<_1>{});
    Tensor gscores_sum = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.scores_sum_ptr) +row_offset_scores_sum),
                            Shape<Int<kBlockM>>{}, Stride<_1>{});
    typename Kernel_traits::GmemTiledCopyO gmem_tiled_copy_O;
    auto gmem_thr_copy_O = gmem_tiled_copy_O.get_thread_slice(tidx);
    Tensor tOsO = gmem_thr_copy_O.partition_S(sO);        // ((Atom,AtomNum),ATOM_M,ATOM_N)
    Tensor tOgO = gmem_thr_copy_O.partition_D(gO);

    __syncthreads();

    //if (cute::thread0()) { printf("fence 0\n"); }

    Tensor tOrO = make_tensor<Element>(shape(tOgO));
    cute::copy(gmem_tiled_copy_O, tOsO, tOrO);
    
    Tensor caccO = make_identity_tensor(Shape<Int<kBlockM>, Int<kHeadDim>>{});    // (BLK_M,BLK_K) -> (blk_m,blk_k)
    Tensor taccOcO = thr_mma.partition_C(caccO);                           // (MMA,MMA_M,MMA_K)
    static_assert(decltype(size<0>(taccOcO))::value == 4);
    // Convert to ((2, 2), MMA_M, MMA_K) then take only the row indices.
    Tensor taccOcO_row = logical_divide(taccOcO, Shape<_2>{})(make_coord(0, _), _, 0);
    CUTE_STATIC_ASSERT_V(size(lse) == size(taccOcO_row));                     // MMA_M
    if (get<1>(taccOcO_row(0)) == 0) {
        #pragma unroll
        for (int mi = 0; mi < size(lse); ++mi) {
            const int row = get<0>(taccOcO_row(mi));
            if (row < binfo.actual_seqlen_q - m_block * kBlockM) { 
                //if (tidx == 0) { printf("m_block=%d, gscore_max row=%d, mi=%d\n",  m_block, row, mi);  }
                gLSE(row) = lse(mi); 
                // Bae: store gscore_max and gscore_sum when m_block > N/2
                if(m_block + 1 > (((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) / 2) + 1){
                    gscores_max(row) = scores_max(mi); 
                    gscores_sum(row) = scores_sum(mi); 
                }
            }
        }
    }
    
    // Construct identity layout for sO
    Tensor cO = make_identity_tensor(make_shape(size<0>(sO), size<1>(sO)));    // (BLK_M,BLK_K) -> (blk_m,blk_k)
    // Repeat the partitioning with identity layouts
    Tensor tOcO = gmem_thr_copy_O.partition_D(cO);                           // (ACPY,ACPY_M,ACPY_K) -> (blk_m,blk_k)
    Tensor tOpO = make_tensor<bool>(make_shape(size<2>(tOgO)));
    if (!Is_even_K) {
        #pragma unroll
        for (int k = 0; k < size(tOpO); ++k) { tOpO(k) = get<1>(tOcO(0, 0, k)) < params.d; }
    }
    // Clear_OOB_K must be false since we don't want to write zeros to gmem
    flash::copy<false, Is_even_K, false, false>(
        gmem_tiled_copy_O, tOrO, tOgO, tOcO, tOpO, binfo.actual_seqlen_q - m_block * kBlockM
    );

    /**/
    __syncthreads();
    if(m_block + 1 > (((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) / 2) + 1 && tidx == 0)
        atomicOr(&CompleteMask[bidh][bidb][blockIdx.x], 1);
    

    //if (cute::thread0()) { printf("fence 1\n"); }
    /*
    if (m_block == ((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) - 1 && tidx == 66) 
    { 
        printf("gscores_max:\n");
        print(gscores_max);
        printf("gscores_sum:\n");
        print(gscores_sum);
        printf("gO:\n");
        print(gO);
    }

    __threadfence();
    atomicAnd(&CompleteMask, 0);
    if (tidx == 0) {
        while ((atomicOr(&CompleteMask, 1ULL << block_id)) != SollMask);
    }
    __syncthreads();    */
    
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //Bae: After complete own blocks, we should compute ptr(N-m_block) blocks fragment from 1 to d/2-f(m_block) when m_block < N/2
    //     This part of result should be store at reg
    //     We should also keep the scores_max and scores_sum in reg

    int reverse_m_block = ((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) - m_block - 1;

    if(m_block + 1 < (((binfo.actual_seqlen_q + kBlockM - 1) / kBlockM) + 1) / 2){
            
        n_block_max = cute::ceil_div(binfo.actual_seqlen_k, kBlockN);
        n_block_max = std::min(n_block_max, cute::ceil_div((reverse_m_block + 1) * kBlockM, kBlockN));
        n_block = n_block_max - 1;

        //dst = cute::ceil_div(binfo.actual_seqlen_k, kBlockN) / 2 + 1;
        dst = cute::ceil_div(kBlockM, kBlockN) * ((cute::ceil_div(binfo.actual_seqlen_q, kBlockM) / 2) + 1);

        /*if (cute::thread0()) { printf("n_block_max=%d\n", n_block_max); 
        printf("binfo.actual_seqlen_k=%d\n", binfo.actual_seqlen_k); 
        printf("kBlockN=%d\n", kBlockN); 
        printf("m_block=%d\n", m_block);
        printf("kBlockM=%d\n", kBlockM); 
        }*/
        //Bae: recompute pointers to ptr(N-m_block) blocks fragment
        const index_t row_offset_q_frag = binfo.q_offset(params.q_batch_stride, params.q_row_stride, bidb)
            + reverse_m_block * kBlockM * params.q_row_stride + bidh * params.q_head_stride;
        // We move K and V to the last block.
        const index_t row_offset_k_frag = binfo.k_offset(params.k_batch_stride, params.k_row_stride, bidb)
            + (n_block_max - 1) * kBlockN * params.k_row_stride + (bidh / params.h_h_k_ratio) * params.k_head_stride;
        const index_t row_offset_v_frag = binfo.k_offset(params.v_batch_stride, params.v_row_stride, bidb)
            + (n_block_max - 1) * kBlockN * params.v_row_stride + (bidh / params.h_h_k_ratio) * params.v_head_stride;
        const index_t row_offset_p_frag = ((bidb * params.h + bidh) * params.seqlen_q_rounded
            + reverse_m_block * kBlockM) * params.seqlen_k_rounded + (n_block_max - 1) * kBlockN;

        gQ = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.q_ptr) + row_offset_q_frag),
                                Shape<Int<kBlockM>, Int<kHeadDim>>{},
                                make_stride(params.q_row_stride, _1{}));
        gK = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.k_ptr) + row_offset_k_frag),
                                Shape<Int<kBlockN>, Int<kHeadDim>>{},
                                make_stride(params.k_row_stride, _1{}));
        gV = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.v_ptr) + row_offset_v_frag),
                                Shape<Int<kBlockN>, Int<kHeadDim>>{},
                                make_stride(params.v_row_stride, _1{}));
        gP = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.p_ptr) + row_offset_p_frag),
                                Shape<Int<kBlockM>, Int<kBlockN>>{},
                                make_stride(params.seqlen_k_rounded, _1{}));
        //if (cute::thread0()) { printf("fence 1.5\n"); }
        //sQ = make_tensor(make_smem_ptr(reinterpret_cast<Element *>(smem_)),
        //                        typename Kernel_traits::SmemLayoutQ{});
        // Careful we're using the same smem for sQ and sK | sV if Share_Q_K_smem;
        //sK = make_tensor(sQ.data() + (Kernel_traits::Share_Q_K_smem ? 0 : size(sQ)),
        //                        typename Kernel_traits::SmemLayoutKV{});
        //sV = make_tensor(sK.data() + size(sK), typename Kernel_traits::SmemLayoutKV{});
        //sVt = make_tensor(sV.data(), typename Kernel_traits::SmemLayoutVtransposed{});
        //sVtNoSwizzle = make_tensor(sV.data(), typename Kernel_traits::SmemLayoutVtransposedNoSwizzle{});

        tQgQ = gmem_thr_copy_QKV.partition_S(gQ);
        tQsQ = gmem_thr_copy_QKV.partition_D(sQ);
        tKgK = gmem_thr_copy_QKV.partition_S(gK);  // (KCPY, KCPY_N, KCPY_K)
        tKsK = gmem_thr_copy_QKV.partition_D(sK);
        tVgV = gmem_thr_copy_QKV.partition_S(gV);  // (VCPY, VCPY_N, VCPY_K)
        tVsV = gmem_thr_copy_QKV.partition_D(sV);
        tPgP = gmem_thr_copy_P.partition_D(gP);

        tSrQ  = thr_mma.partition_fragment_A(sQ);                           // (MMA,MMA_M,MMA_K)
        tSrK  = thr_mma.partition_fragment_B(sK);                           // (MMA,MMA_N,MMA_K)
        tOrVt  = thr_mma.partition_fragment_B(sVtNoSwizzle);                // (MMA, MMA_K,MMA_N)

        //auto smem_tiled_copy_Q = make_tiled_copy_A(typename Kernel_traits::SmemCopyAtom{}, tiled_mma);
        //auto smem_thr_copy_Q = smem_tiled_copy_Q.get_thread_slice(tidx);
        // auto smem_thr_copy_Q = make_tiled_copy_A_warpcontiguousM<MMA_M>(typename Kernel_traits::SmemCopyAtom{}, tiled_mma).get_thread_slice(tidx);
        // if (cute::thread0()) {smem_thr_copy_Q.print_all();}
        tSsQ = smem_thr_copy_Q.partition_S(sQ);
        // if (cute::thread0()) {print(tSsQ.layout()); printf("\n");}

        //auto smem_tiled_copy_K = make_tiled_copy_B(typename Kernel_traits::SmemCopyAtom{}, tiled_mma);
        //auto smem_thr_copy_K = smem_tiled_copy_K.get_thread_slice(tidx);
        tSsK = smem_thr_copy_K.partition_S(sK);

        //auto smem_tiled_copy_V = make_tiled_copy_B(typename Kernel_traits::SmemCopyAtomTransposed{}, tiled_mma);
        //auto smem_thr_copy_V = smem_tiled_copy_V.get_thread_slice(tidx);
        tOsVt = smem_thr_copy_V.partition_S(sVt);
        //Bae: acc_o size is (4, B_r/2, Headdim/2)
        //Tensor acc_o = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kHeadDim>>{});  // MMA, MMA_M, MMA_K

        // Prologue

        tQrQ = make_fragment_like(tQgQ);
        // We don't need to clear the sQ smem tiles since we'll only write out the valid outputs
        flash::copy<false, Is_even_K>(gmem_tiled_copy_QKV, tQgQ, tQsQ, tQcQ, tQpQ,
                                                    binfo.actual_seqlen_q - reverse_m_block * kBlockM);
        if (Kernel_traits::Is_Q_in_regs) { cute::cp_async_fence(); }

        // // Copy rmem to smem
        // // copy(tQrQ, tQsQ);
        // flash::cp_async_wait<0>();
        // __syncthreads();
        // // if (cute::thread(1, 0)) { print(tQsQ); }
        // // Tensor sQNoSwizzle = make_tensor(make_smem_ptr(reinterpret_cast<Element *>(smem_)), typename Kernel_traits::SmemLayoutQNoSwizzle{});
        // // if (cute::thread0()) { print(sQNoSwizzle); }

        if (Kernel_traits::Share_Q_K_smem) {
            flash::cp_async_wait<0>();
            __syncthreads();
            Tensor tSrQ_copy_view = smem_thr_copy_Q.retile_D(tSrQ);
            CUTE_STATIC_ASSERT_V(size<1>(tSsQ) == size<1>(tSrQ_copy_view));            // M
            cute::copy(smem_tiled_copy_Q, tSsQ, tSrQ_copy_view);
            __syncthreads();
        }

        // We don't need to clear the sK smem tiles since we'll mask out the scores anyway.
        flash::copy<Is_even_N, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKVcKV, tKVpKV,
                                        binfo.actual_seqlen_k - n_block * kBlockN);
        cute::cp_async_fence();
        // if (threadIdx.x == 0 && blockIdx.y == 0 && blockIdx.z < 2) { print(tKgK); }
        // __syncthreads();

        if (Kernel_traits::Is_Q_in_regs && !Kernel_traits::Share_Q_K_smem) {
            flash::cp_async_wait<1>();
            __syncthreads();
            Tensor tSrQ_copy_view = smem_thr_copy_Q.retile_D(tSrQ);
            CUTE_STATIC_ASSERT_V(size<1>(tSsQ) == size<1>(tSrQ_copy_view));            // M
            cute::copy(smem_tiled_copy_Q, tSsQ, tSrQ_copy_view);
        }

        //auto seeds = at::cuda::philox::unpack(params.philox_args);
        //unsigned long long seed = std::get<0>(seeds);
        //Bae: maybe tidx should be changed, but seems not important to result
        unsigned long long offset = std::get<1>(seeds) + (bidb * params.h + bidh) * 32 + tidx % 32; 

        // Save seed and offset for backward.
        //if (block_id == 0 && tidx == 0) {
        //    params.rng_state[0] = seed;
        //    params.rng_state[1] = std::get<1>(seeds);
        //}

        //if (cute::thread0()) { printf("fence 1.75\n"); }

        clear(acc_o);
        clear(scores_max);
        clear(scores_sum);
        
        //if (cute::thread0()) { printf("fence 1.8\n"); }

        // For performance reason, we separate out two kinds of iterations:
        // those that need masking on S, and those that don't.
        // We need masking on S for the very last block when K and V has length not multiple of kBlockN.
        // We also need masking on S if it's causal, for the last ceil_div(kBlockM, kBlockN) blocks.
        // We will have at least 1 "masking" iteration.

        //Bae: n_masking_steps controls the masking of very last block, n_block control the whole number of masking blocks

        constexpr int n_masking_steps = cute::ceil_div(kBlockM, kBlockN); //Bae: this is for the last causal block
        #pragma unroll
        for (int masking_step = 0; masking_step < n_masking_steps; ++masking_step, --n_block) {
            Tensor acc_s = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kBlockN>>{});
            clear(acc_s);
            flash::cp_async_wait<0>();
            __syncthreads();

            // Advance gV
            if (masking_step > 0) {
                tVgV.data() = tVgV.data() + (-int(kBlockN * params.v_row_stride));
                flash::copy<true, Is_even_K>(gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV);
            } else {
                // Clear the smem tiles to account for predicated off loads
                flash::copy<Is_even_N, Is_even_K, true>(
                    gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV, binfo.actual_seqlen_k - n_block * kBlockN
                );
            }
            cute::cp_async_fence();

            flash::gemm<Kernel_traits::Is_Q_in_regs>(
                acc_s, tSrQ, tSrK, tSsQ, tSsK, tiled_mma, smem_tiled_copy_Q, smem_tiled_copy_K,
                smem_thr_copy_Q, smem_thr_copy_K
            );
            // if (cute::thread0()) { print(acc_s); }

            // Reshape acc_s from (MMA=4, MMA_M, MMA_N) to (nrow=(2, MMA_M), ncol=(2, MMA_N))
            Tensor scores = make_tensor(acc_s.data(), flash::convert_layout_acc_rowcol(acc_s.layout()));
            // if (cute::thread0()) { print(scores); }
            // We don't put the masking before the matmul S = Q K^T because we don't clear sK
            // for rows outside actual_seqlen_k. So those rows could have Inf / NaN, and the matmul
            // can produce Inf / NaN.

                // Tensor caccS = make_identity_tensor(Shape<Int<kBlockM>, Int<kBlockN>>{});    // (BLK_M,BLK_N) -> (blk_m,blk_n)
                // Tensor taccScS = thr_mma.partition_C(caccS);                           // (MMA,MMA_M,MMA_N)
                // static_assert(decltype(size<0>(taccScS))::value == 4);
                // // Convert to ((2, 2), MMA_M, MMA_N) then take only the row indices.
                // Tensor idx_row = logical_divide(taccScS, Shape<_2>{})(make_coord(0, _), _, 0);
                // Tensor idx_rowcol = make_tensor(taccScS.data(), flash::convert_layout_acc_rowcol(taccScS.layout()));
                // flash::apply_mask_causal_w_idx(scores, idx_rowcol, n_block * kBlockN, binfo.actual_seqlen_k,
                //                               m_block * kBlockM);
                // Idk why it's get<1> and not get<0> of the stride.
                // if (cute::thread0()) { print(idx_row.layout()); print(stride<1>(idx_row)); printf("stride = %d \n", get<1>(stride<1>(idx_row))); }
                // I can't get the stride from idx_row
                flash::apply_mask_causal(scores, n_block * kBlockN, binfo.actual_seqlen_k,
                                        // m_block * kBlockM + get<0>(idx_row(0)),
                                        reverse_m_block * kBlockM + (tidx / 32) * 16 + (tidx % 32) / 4,
                                        kNWarps * 16);
                                        // m_block * kBlockM + (tidx / 32) * 16, kNWarps * 16);
                                        // m_block * kBlockM + (tidx / 32) * (kBlockM / kNWarps), 16);

            flash::cp_async_wait<0>();
            __syncthreads();
            if (n_block > dst) {
                // Advance gK
                tKgK.data() = tKgK.data() + (-int(kBlockN * params.k_row_stride));
                flash::copy<true, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKVcKV, tKVpKV);
                // This cp_async_fence needs to be in the if block, otherwise the synchronization
                // isn't right and we get race conditions.
                cute::cp_async_fence();
            }

            // TODO: when we have key_padding_mask we'll need to Check_inf
            masking_step == 0
                ? softmax_rescale_o<true,  Is_causal>(scores, scores_max, scores_sum, acc_o, params.scale_softmax_log2)
                : softmax_rescale_o<false, Is_causal>(scores, scores_max, scores_sum, acc_o, params.scale_softmax_log2);

            // Convert scores from fp32 to fp16/bf16
            Tensor rP = flash::convert_type<Element>(scores);
            // Reshape rP from (nrow=(2, MMA_M), ncol=(2, MMA_N)) to ((2, 2, 2), MMA_M, MMA_N / 2)
            // if using m16n8k16 or ((2, 2, 1), MMA_M, MMA_N) if using m16n8k8.
            Tensor tOrP = make_tensor(rP.data(), flash::convert_layout_rowcol_Aregs<Kernel_traits::TiledMma>(rP.layout()));
            uint32_t block_row_idx = reverse_m_block * (kBlockM / 16) + tidx / 32;
            uint32_t block_col_idx = n_block * (kBlockN / 32);
            if (Return_softmax) {
                Tensor tOrP_copy = make_fragment_like(tOrP);
                cute::copy(tOrP, tOrP_copy);
                flash::apply_dropout<true>(
                    tOrP_copy, params.p_dropout_in_uint8_t, seed, offset,
                    block_row_idx, block_col_idx, kNWarps
                );
                flash::write_softmax_to_gmem(tOrP_copy, tPgP, gmem_tiled_copy_P);
                tPgP.data() = tPgP.data() + (-kBlockN);
            }
            if (Is_dropout) {
                flash::apply_dropout(tOrP, params.p_dropout_in_uint8_t, seed, offset,
                                    block_row_idx, block_col_idx, kNWarps);
            }
            // if (cute::thread0()) { print(tOrP); }

            flash::gemm_A_in_regs(acc_o, tOrP, tOrVt, tOsVt, tiled_mma, smem_tiled_copy_V, smem_thr_copy_V);
            // if (cute::thread0()) { print(scores); }

            // This check is at the end of the loop since we always have at least 1 iteration
            if (n_masking_steps > 1 && n_block <= dst) {
                --n_block;
                break;
            }
        }

        //if (cute::thread0()) { printf("fence -3\n"); }

        // These are the iterations where we don't need masking on S
        for (; n_block >= dst; --n_block) {
            Tensor acc_s = partition_fragment_C(tiled_mma, Shape<Int<kBlockM>, Int<kBlockN>>{});  // (MMA=4, MMA_M, MMA_N)
            clear(acc_s);
            flash::cp_async_wait<0>();
            __syncthreads();
            // Advance gV
            tVgV.data() = tVgV.data() + (-int(kBlockN * params.v_row_stride));
            flash::copy<true, Is_even_K>(gmem_tiled_copy_QKV, tVgV, tVsV, tKVcKV, tKVpKV);
            cute::cp_async_fence();

            flash::gemm<Kernel_traits::Is_Q_in_regs>(
                acc_s, tSrQ, tSrK, tSsQ, tSsK, tiled_mma, smem_tiled_copy_Q, smem_tiled_copy_K,
                smem_thr_copy_Q, smem_thr_copy_K
            );


            flash::cp_async_wait<0>();
            __syncthreads();
            if (n_block > dst) {
                // Advance gK
                tKgK.data() = tKgK.data() + (-int(kBlockN * params.k_row_stride));
                flash::copy<true, Is_even_K>(gmem_tiled_copy_QKV, tKgK, tKsK, tKVcKV, tKVpKV);
                // This cp_async_fence needs to be in the if block, otherwise the synchronization
                // isn't right and we get race conditions.
                cute::cp_async_fence();
            }

            // Reshape acc_s from (MMA=4, MMA_M, MMA_N) to (nrow=(2, MMA_M), ncol=(2, MMA_N))
            Tensor scores = make_tensor(acc_s.data(), flash::convert_layout_acc_rowcol(acc_s.layout()));
            softmax_rescale_o<false>(scores, scores_max, scores_sum, acc_o, params.scale_softmax_log2);

            Tensor rP = flash::convert_type<Element>(scores);
            // Reshape rP from (nrow=(2, MMA_M), ncol=(2, MMA_N)) to ((2, 2, 2), MMA_M, MMA_N / 2)
            // if using m16n8k16 or ((2, 2, 1), MMA_M, MMA_N) if using m16n8k8.
            Tensor tOrP = make_tensor(rP.data(), flash::convert_layout_rowcol_Aregs<Kernel_traits::TiledMma>(rP.layout()));
            uint32_t block_row_idx = reverse_m_block * (kBlockM / 16) + tidx / 32;
            uint32_t block_col_idx = n_block * (kBlockN / 32);
            if (Return_softmax) {
                Tensor tOrP_copy = make_fragment_like(tOrP);
                cute::copy(tOrP, tOrP_copy);
                flash::apply_dropout<true>(
                    tOrP_copy, params.p_dropout_in_uint8_t, seed, offset,
                    block_row_idx, block_col_idx, kNWarps
                );
                flash::write_softmax_to_gmem(tOrP_copy, tPgP, gmem_tiled_copy_P);
                tPgP.data() = tPgP.data() + (-kBlockN);
            }
            if (Is_dropout) {
                flash::apply_dropout(tOrP, params.p_dropout_in_uint8_t, seed, offset,
                                    block_row_idx, block_col_idx, kNWarps);
            }
            flash::gemm_A_in_regs(acc_o, tOrP, tOrVt, tOsVt, tiled_mma, smem_tiled_copy_V, smem_thr_copy_V);
        }

        //if (cute::thread0()) { printf("fence 1.875\n"); }

        /*if (m_block == 0 && tidx == 66) 
        { 
            printf("fragment:\n");
            printf("scores_max:\n");
            print(scores_max);
            printf("scores_sum:\n");
            print(scores_sum);
            printf("acc_o:\n");
            print(acc_o);
        }*/
        //if (cute::thread0()) { printf("fence 1.9\n"); }
        // Epilogue

        // Reshape acc_o from (MMA=4, MMA_M, MMA_K) to (nrow=(2, MMA_M), ncol=(2, MMA_K))
        // Tensor acc_o_rowcol = make_tensor(acc_o.data(), flash::convert_layout_acc_rowcol(acc_o.layout()));
        // Tensor lse = make_fragment_like(scores_sum);
        clear(lse);

        #pragma unroll
        for (int mi = 0; mi < size<0>(acc_o_rowcol); ++mi) {
            float sum = scores_sum(mi);
            float inv_sum = (sum == 0.f || sum != sum) ? 1.f : 1.f / sum;
            lse(mi) = (sum == 0.f || sum != sum) ? INFINITY : scores_max(mi) * params.scale_softmax + __logf(sum);
            float scale = !Is_dropout ? inv_sum : inv_sum * params.rp_dropout;
            #pragma unroll
            for (int ni = 0; ni < size<1>(acc_o_rowcol); ++ni) { acc_o_rowcol(mi, ni) *= scale; }
        }
        /*
        __threadfence();
        atomicAnd(&CompleteMask, 0);
        if (tidx == 0) {
            while ((atomicOr(&CompleteMask, 1ULL << block_id)) != SollMask);
        }
        __syncthreads();
        if (m_block == 0 && tidx == 66) 
        { 
            printf("fragment:\n");
            printf("scores_max:\n");
            print(scores_max);
            printf("scores_sum:\n");
            print(scores_sum);
            printf("acc_o:\n");
            print(acc_o);
        }*/

        //if (cute::thread0()) { printf("fence 2\n"); }

        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        //Bae: we should sync for the whole token. In flash attention, one block is assigned to one SM. So the sync range is a few block in a few SMs.
        //     __threadfence() sync for grid level. Maybe it is too wide because it will wait all the blocks, 
        //     and the whole grid maybe not on the device simultaneously because it is too large.
        //     But I don't know how to sync with some blocks, maybe we can point out which SM to assign the block by CUDA APIs.
        //     Or we can use CUDA cooperative groups API. (seems only supported by Hopper arch?)
        
        //__threadfence();
        //atomicAnd(&CompleteMask, 0);

        /**/
        if (tidx == 0) {
            while(atomicCAS(&CompleteMask[bidh][bidb][m_block], 0, 0) != 1);
        }
        __syncthreads();
        
        /*__threadfence();
        atomicAnd(&CompleteMask, 0);
        if (tidx == 0) {
            while ((atomicOr(&CompleteMask, 1ULL << block_id)) != SollMask);
        }
        __syncthreads();*/
        //if (cute::thread0()) { printf("fence 3\n"); }

        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        //Bae: Then we should merge two fragments when m_block < N/2, fragment 1 to d/2-f(m_block) stored at shared memory, 
        //     d/2-f(m_block) to d-f(m_block) stored at global memory. Fragment in glb memmory will be load to shared memmory, 
        //     after merging the result will be load to global memory (the same place as where stored fragment). 
            
        //Bae: fragment 1 to d/2-f(m_block) stored at sO, d/2-f(m_block) to d-f(m_block) stored at global mem somewhere, we need to recompute pointers
        const index_t row_offset_frag_scores_max = (bidb * params.h + bidh) * params.seqlen_q + reverse_m_block * kBlockM;
        const index_t row_offset_frag_scores_sum = (bidb * params.h + bidh) * params.seqlen_q + reverse_m_block * kBlockM;
        gscores_max = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.scores_max_ptr) +row_offset_frag_scores_max),
                                Shape<Int<kBlockM>>{}, Stride<_1>{});
        gscores_sum = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.scores_sum_ptr) +row_offset_frag_scores_sum),
                                Shape<Int<kBlockM>>{}, Stride<_1>{});
        
        const index_t row_offset_o_frag = binfo.q_offset(params.o_batch_stride, params.o_row_stride, bidb)
            + reverse_m_block * kBlockM * params.o_row_stride + bidh * params.o_head_stride;
        Tensor gOf = make_tensor(make_gmem_ptr(reinterpret_cast<Element *>(params.o_ptr) + row_offset_o_frag),
                            Shape<Int<kBlockM>, Int<kHeadDim>>{},
                            make_stride(params.o_row_stride, _1{}));
        const index_t row_offset_lse_frag = (bidb * params.h + bidh) * params.seqlen_q + reverse_m_block * kBlockM;
        gLSE = make_tensor(make_gmem_ptr(reinterpret_cast<ElementAccum *>(params.softmax_lse_ptr) + row_offset_lse_frag),
                            Shape<Int<kBlockM>>{}, Stride<_1>{});
        //Bae: reload fragment from g0f to shared mem sOf (O is the same size as Q, so the copy operation is similar), stores at sQ address
        //Tensor sOf = make_tensor(sK.data() + (Kernel_traits::Share_Q_K_smem ? size(sK) : 0), typename Kernel_traits::SmemLayoutO{});    // (SMEM_M,SMEM_N)
        Tensor sOf = make_tensor(sQ.data(), typename Kernel_traits::SmemLayoutO{});    // (SMEM_M,SMEM_N)

        //typename Kernel_traits::GmemTiledCopyO gmem_tiled_copy_O;
        gmem_thr_copy_O = gmem_tiled_copy_O.get_thread_slice(tidx);

        Tensor tOgOf = gmem_thr_copy_O.partition_S(gOf);
        Tensor tOsOf = gmem_thr_copy_O.partition_D(sOf);
        
        Tensor tOrOf = make_tensor<Element>(shape(tOgOf));
        __syncthreads();

        // PREDICATES

        // Construct identity layout 
        Tensor cOf = make_identity_tensor(make_shape(size<0>(sOf), size<1>(sOf)));    // (BLK_M,BLK_K) -> (blk_m,blk_k)

        // Repeat the partitioning with identity layouts
        Tensor tOcOf = gmem_thr_copy_O.partition_S(cOf);       // (ACPY,ACPY_M,ACPY_K) -> (blk_m,blk_k)

        // Allocate predicate tensors for k
        Tensor tOpOf = make_tensor<bool>(make_shape(size<2>(tOsOf)));

        // Set predicates for k bounds
        if (!Is_even_K) {
            #pragma unroll
            for (int k = 0; k < size(tOpOf); ++k) { tOpOf(k) = get<1>(tOcOf(0, 0, k)) < params.d; }
        }

        //if (cute::thread0()) { printf("fence 5\n"); }

        // We don't need to clear the sQ smem tiles since we'll only write out the valid outputs
        flash::copy<false, Is_even_K, false, false>(
            gmem_tiled_copy_O, tOgOf, tOrOf, tOcOf, tOpOf, binfo.actual_seqlen_q - reverse_m_block * kBlockM);
        __syncthreads(); 
        cute::copy(gmem_tiled_copy_O, tOrOf, tOsOf);
        __syncthreads(); 
        Tensor rOf = make_fragment_like(acc_o);

        Tensor taccOrOf = smem_thr_copy_O.retile_D(rOf);        // ((Atom,AtomNum), MMA_M, MMA_N)
        Tensor taccOsOf = smem_thr_copy_O.partition_S(sOf);     // ((Atom,AtomNum),PIPE_M,PIPE_N)

        // sO has the same size as sQ, so we don't need to sync here.
        if (Kernel_traits::Share_Q_K_smem) { __syncthreads(); }

        //if (cute::thread0()) { printf("fence 6\n"); }

        cute::copy(smem_tiled_copy_O, taccOsOf, taccOrOf);
        /*if (m_block == 0 && tidx == 66) 
        { 
            printf("fence 6.6\n");
            printf("tOgOf:\n");
            print(tOgOf);
            printf("tOrOf:\n");
            print(tOrOf);
            printf("tOsOf:\n");
            print(tOsOf);
            printf("taccOsOf:\n");
            print(taccOsOf);
            printf("taccOrOf:\n");
            print(taccOrOf);
        }*/
        //Bae: We need to store and load score_max and score_sum. So new memory should be assign to score_max and score_sum.
        //     For each block, the size is KblockM * 1.

        Tensor fragment_scores_max = make_tensor<ElementAccum>(Shape<Int<2 * size<1>(acc_o)>>{});
        Tensor fragment_scores_sum = make_fragment_like(fragment_scores_max);

        caccO = make_identity_tensor(Shape<Int<kBlockM>, Int<kHeadDim>>{});    // (BLK_M,BLK_K) -> (blk_m,blk_k)
        taccOcO = thr_mma.partition_C(caccO);                           // (MMA,MMA_M,MMA_K)
        //static_assert(decltype(size<0>(taccOcO))::value == 4);
        // Convert to ((2, 2), MMA_M, MMA_K) then take only the row indices.
        taccOcO_row = logical_divide(taccOcO, Shape<_2>{})(make_coord(0, _), _, 0);
        //CUTE_STATIC_ASSERT_V(size(lse) == size(taccOcO_row));                     // MMA_M
        /*if (m_block == 0 && tidx == 66) {
            printf("get<1>(taccOcO_row(0))=%d\n", get<1>(taccOcO_row(0)));
            printf("size(taccOcO_row(0))=%d\n", size(taccOcO_row(0)));
            printf("rank(taccOcO_row(0))=%d\n", rank(taccOcO_row(0)));
            printf("size(taccOcO_row)=%d\n", size(taccOcO_row));
            printf("rank(taccOcO_row)=%d\n", rank(taccOcO_row));
            //print(taccOcO_row);
        }*/
        //if (get<1>(taccOcO_row(0)) == 0) {
            #pragma unroll
            for (int mi = 0; mi < size(lse); ++mi) {
                const int row = get<0>(taccOcO_row(mi));
                /*if (tidx == 66) { 
                    printf("reverse_m_block=%d, frag_gscore_max row=%d, mi=%d\n", reverse_m_block, row, mi);
                }*/
                if (row < binfo.actual_seqlen_q - reverse_m_block * kBlockM) {
                    fragment_scores_max(mi) = gscores_max(row); 
                    fragment_scores_sum(mi) = gscores_sum(row); 
                }
            }
        //}

        //Bae: Merge. Result stores at acc_o, scores_max, scores_sum

        /*if (m_block == 0 && tidx == 66) 
        { 
            printf("fence 7\n");
            printf("scores_max:\n");
            print(scores_max);
            printf("scores_sum:\n");
            print(scores_sum);
            printf("acc_o:\n");
            print(acc_o);
            printf("fragment_scores_max:\n");
            print(fragment_scores_max);
            printf("fragment_scores_sum:\n");
            print(fragment_scores_sum);
            printf("rOf:\n");
            print(rOf);
        }*/
        softmax_merge_o<false>(scores_max, scores_sum, fragment_scores_max, fragment_scores_sum, acc_o, rOf, params.scale_softmax_log2);

        /*if (m_block == 0 && tidx == 66) 
        { 
            printf("fence 7.5 merge result\n");
            printf("scores_max:\n");
            print(scores_max);
            printf("scores_sum:\n");
            print(scores_sum);
            printf("rOf:\n");
            print(rOf);         
        }*/

        //Bae: Re-compute LSE

        Tensor rOf_rowcol = make_tensor(rOf.data(), flash::convert_layout_acc_rowcol(rOf.layout()));
        //Tensor lse = make_fragment_like(scores_sum);
        clear(lse);

        #pragma unroll
        for (int mi = 0; mi < size<0>(rOf_rowcol); ++mi) {
            float sum = scores_sum(mi);
            //float inv_sum = (sum == 0.f || sum != sum) ? 1.f : 1.f / sum;
            lse(mi) = (sum == 0.f || sum != sum) ? INFINITY : scores_max(mi) * params.scale_softmax + __logf(sum);
        }

        //Bae: load final result to glb_mem

        //if (cute::thread0()) { printf("fence 8\n"); }

        // sO has the same size as sQ, so we don't need to sync here.

        Tensor rOf_store = flash::convert_type<Element>(rOf);

        Tensor taccOrOf_store = smem_thr_copy_O.retile_S(rOf_store);        // ((Atom,AtomNum), MMA_M, MMA_N)
        Tensor taccOsOf_store = smem_thr_copy_O.partition_D(sOf);     // ((Atom,AtomNum),PIPE_M,PIPE_N)

        if (Kernel_traits::Share_Q_K_smem) { __syncthreads(); }

        cute::copy(smem_tiled_copy_O, taccOrOf_store, taccOsOf_store);

        Tensor tOgOf_store = gmem_thr_copy_O.partition_D(gOf);
        Tensor tOsOf_store = gmem_thr_copy_O.partition_S(sOf);
        __syncthreads();
        Tensor tOrOf_store = make_tensor<Element>(shape(tOgOf_store));

        cute::copy(gmem_tiled_copy_O, tOsOf_store, tOrOf_store);

        //Tensor caccO = make_identity_tensor(Shape<Int<kBlockM>, Int<kHeadDim>>{});    // (BLK_M,BLK_K) -> (blk_m,blk_k)
        //Tensor taccOcO = thr_mma.partition_C(caccO);                           // (MMA,MMA_M,MMA_K)
        //static_assert(decltype(size<0>(taccOcO))::value == 4);
        // Convert to ((2, 2), MMA_M, MMA_K) then take only the row indices.
        //Tensor taccOcO_row = logical_divide(taccOcO, Shape<_2>{})(make_coord(0, _), _, 0);
        //CUTE_STATIC_ASSERT_V(size(lse) == size(taccOcO_row));                     // MMA_M
        //if (get<1>(taccOcO_row(0)) == 0) {
            #pragma unroll
            for (int mi = 0; mi < size(lse); ++mi) {
                const int row = get<0>(taccOcO_row(mi));
                if (row < binfo.actual_seqlen_q - reverse_m_block * kBlockM) { 
                    gLSE(row) = lse(mi); 
                }
            }
        //}

        //if (cute::thread0()) { printf("fence 9\n"); }

        // Construct identity layout for sO
        Tensor cOf_store = make_identity_tensor(make_shape(size<0>(sOf), size<1>(sOf)));    // (BLK_M,BLK_K) -> (blk_m,blk_k)
        // Repeat the partitioning with identity layouts
        Tensor tOcOf_store = gmem_thr_copy_O.partition_D(cOf_store);       // (ACPY,ACPY_M,ACPY_K) -> (blk_m,blk_k)
        // Allocate predicate tensors for k
        Tensor tOpOf_store = make_tensor<bool>(make_shape(size<2>(tOgOf_store)));
        
        if (!Is_even_K) {
            #pragma unroll
            for (int k = 0; k < size(tOpOf_store); ++k) { tOpOf_store(k) = get<1>(tOcOf_store(0, 0, k)) < params.d; }
        }
        // Clear_OOB_K must be false since we don't want to write zeros to gmem
        flash::copy<false, Is_even_K, false, false>(
            gmem_tiled_copy_O, tOrOf_store, tOgOf_store, tOcOf_store, tOpOf_store, binfo.actual_seqlen_q - reverse_m_block * kBlockM
        );
        /*if (m_block == 0 && tidx == 66) 
        { 
            printf("fence 9.9\n");
            printf("tOgOf_store:\n");
            print(tOgOf_store);
            printf("tOrOf_store:\n");
            print(tOrOf_store);
            printf("tOsOf:\n");
            print(tOsOf);
            printf("taccOrOf_store:\n");
            print(taccOrOf_store);
            printf("taccOsOf_store:\n");
            print(taccOsOf_store);
        }*/
    }
    /**/
    // uint64_t start = clock64();
    // uint64_t end = start + (uint64_t)(1000000000);
    // while(clock64() < end) {
    //         float dummy = 0.0;
    //         for(int i=0;i<1000;i++){
    //                 dummy += tanf(dummy) + logf(dummy);
    //         }
    // }
    /*
    uint64_t end_time = GlobalTimer64();
    if(threadIdx.x == 0) {
        printf("block: (%d, %d, %d), sm_id=%u, start_time=%llu, end_time=%llu, exec_time=%llu\n", 
            blockIdx.x, bidb, bidh, sm_id, start_time, end_time, end_time-start_time);
    }
    */
}

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename Kernel_traits, bool Is_dropout, bool Is_causal, bool Is_even_N, bool Is_even_K, bool Return_softmax, typename Params>
inline __device__ void compute_attn(const Params &params) {
    const int m_block = blockIdx.x;
    // The block index for the batch.
    const int bidb = blockIdx.y;
    // The block index for the head.
    const int bidh = blockIdx.z;

    // We want the fwd and bwd to generate the same dropout pattern (RNG), without restricting
    // them to have the same number of threads or have to traverse the attention matrix
    // in the same order.
    // In the Philox RNG, we use the offset to store the batch, head, and the lane id
    // (within a warp). We use the subsequence to store the location of the 16 x 32 blocks within
    // the attention matrix. This way, as long as we have the batch, head, and the location of
    // the 16 x 32 block within the attention matrix, we can generate the exact same dropout pattern.
    flash::compute_attn_1rowblock<Kernel_traits, Is_dropout, Is_causal, Is_even_N, Is_even_K, Return_softmax>(params, bidb, bidh, m_block);
}

template<typename Kernel_traits, bool Is_dropout, bool Is_causal, bool Is_even_N, bool Is_even_K, bool Return_softmax, typename Params>
inline __device__ void compute_attn_casual(const Params &params, const int bidb, const int bidh) {
    const int m_block = gridDim.x - 1 - blockIdx.x;
    // The block index for the batch.
    // const int bidb = blockIdx.y;
    // The block index for the head.
    // const int bidh = blockIdx.z;

    // We want the fwd and bwd to generate the same dropout pattern (RNG), without restricting
    // them to have the same number of threads or have to traverse the attention matrix
    // in the same order.
    // In the Philox RNG, we use the offset to store the batch, head, and the lane id
    // (within a warp). We use the subsequence to store the location of the 16 x 32 blocks within
    // the attention matrix. This way, as long as we have the batch, head, and the location of
    // the 16 x 32 block within the attention matrix, we can generate the exact same dropout pattern.

    if(!Is_causal)
        flash::compute_attn_1rowblock<Kernel_traits, Is_dropout, true/*Is_causal*/, Is_even_N, Is_even_K, Return_softmax>(params, blockIdx.y, blockIdx.z, m_block);
    else
        flash::compute_attn_1rowblock_causal<Kernel_traits, Is_dropout, true/*Is_causal*/, Is_even_N, Is_even_K, Return_softmax>(params, bidb, bidh, m_block);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace flash