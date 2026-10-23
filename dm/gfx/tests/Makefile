# 单测：make 编译并运行（秒级）
# 源文件按通配纳入，口径与宿主 Makefile 的 GFX 一致（hle_nvn.c 由 t_nvn.c 直接 #include，故剔除）；新增源文件无需改这里
# Unit tests: make compiles and runs them (seconds)
# Source files are included by wildcard, same convention as GFX in the host Makefile (hle_nvn.c is #included directly by t_nvn.c, so it is excluded); adding new source files needs no change here
CFLAGS=-O1 -g -Wall -Wno-unused-function -I../nvn/include
GFX=$(filter-out ../nvn/hle_nvn.c,$(sort $(wildcard ../nvn/*.c ../nvn/*.m ../nvn/*.S ../tex/*.c ../tex/*.m)))
GFX_H=$(wildcard ../nvn/*.h ../nvn/*.inc ../nvn/include/*.h ../tex/*.h)
FW=-framework Metal -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers
# 宿主 crash.c 一并链入：写保护单测走生产同一套缺页钩子
# Host crash.c is linked in as well: the write-protect unit tests go through the same page-fault hook as production
t_nvn: t_nvn.c ../nvn/hle_nvn.c ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_nvn.c ../../host/crash.c $(GFX) $(FW)
t_dirty_slots: t_dirty_slots.c ../nvn/nvn_dirty.c ../../host/crash.c ../nvn/nvn_dirty.h
	clang $(CFLAGS) -o $@ t_dirty_slots.c ../../host/crash.c ../nvn/nvn_dirty.c
t_dirty_race: t_dirty_race.c ../nvn/nvn_dirty.c ../../host/crash.c ../nvn/nvn_dirty.h
	clang $(CFLAGS) -o $@ t_dirty_race.c ../../host/crash.c
t_builtin: t_builtin.m ../nvn/nvn_builtin.h
	clang -O1 -g -Wall -fobjc-arc -o $@ t_builtin.m -framework Metal -framework Foundation
t_deswz_gpu: t_deswz_gpu.m ../tex/nvn_deswizzle.c ../tex/nvn_deswizzle_gpu.m ../tex/nvn_deswizzle.h ../tex/nvn_deswizzle_gpu.h ../nvn/nvn_tex.c ../nvn/nvn_tex.h
	clang $(CFLAGS) -fobjc-arc -o $@ t_deswz_gpu.m ../tex/nvn_deswizzle.c ../tex/nvn_deswizzle_gpu.m ../nvn/nvn_tex.c -framework Metal -framework Foundation
t_refl: t_refl.m ../nvn/nvn_refl.m ../nvn/nvn_refl.h ../nvn/dmsh_chain.h
	clang -O1 -g -Wall -fobjc-arc -o $@ t_refl.m ../nvn/nvn_refl.m -framework Metal -framework Foundation
t_hmap: t_hmap.c ../nvn/nvn_hmap.h
	clang -O1 -Wall -o $@ t_hmap.c
t_cover: t_cover.c ../nvn/nvn_cover.h
	clang -O1 -Wall -o $@ t_cover.c
t_pace: t_pace.c ../nvn/nvn_pace.h
	clang -O1 -Wall -o $@ t_pace.c
t_adaptive_pace: t_adaptive_pace.c ../nvn/nvn_adaptive_pace.h ../nvn/nvn_pace.h
	clang -O1 -Wall -o $@ t_adaptive_pace.c
t_bc8: t_bc8.c ../vulkan/engine/xbg_texture.c ../vulkan/engine/xbg_texture.h ../nvn/nvn_tex.c ../tex/nvn_deswizzle.h
	clang -O1 -Wall -Wno-unused-function -fsigned-char -I../vulkan/engine -o $@ t_bc8.c ../vulkan/engine/xbg_texture.c ../nvn/nvn_tex.c -lpthread
t_completion_wait: t_completion_wait.cpp ../vulkan/engine/xbg_completion_wait.h
	clang++ -std=c++17 -O1 -Wall -pthread -DDM_COMPLETION_WAIT_SELFTEST -DDM_COMPLETION_WAIT_MAIN -o $@ t_completion_wait.cpp
t_fuse_check: t_fuse_check.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_fuse_check.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_rt_srgb_view: t_rt_srgb_view.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_rt_srgb_view.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_present_crop: t_present_crop.m ../nvn/hle_nvn.c ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_present_crop.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m,$(GFX)) $(FW)
t_reorder_chain: t_reorder_chain.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_reorder_chain.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_defer_chain: t_defer_chain.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_defer_chain.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_zskip: t_zskip.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_zskip.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_xv: t_xv.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_xv.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_ub: t_ub.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_ub.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_mip_extent: t_mip_extent.m ../nvn/hle_nvn.c ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_mip_extent.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m,$(GFX)) $(FW)
t_rt_range_shape: t_rt_range_shape.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_rt_range_shape.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_rt_range_ro: t_rt_range_ro.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_rt_range_ro.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_rt_range: t_rt_range.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_rt_range.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_rt_depth_ro: t_rt_depth_ro.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_rt_depth_ro.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
t_pso_skey: t_pso_skey.m ../nvn/nvn_pso_skey.h ../nvn/nvn_obj.h
	clang $(CFLAGS) -fobjc-arc -o $@ t_pso_skey.m -framework Foundation
t_pfv: t_pfv.m ../../host/crash.c $(GFX) $(GFX_H)
	clang $(CFLAGS) -fobjc-arc -o $@ t_pfv.m ../../host/crash.c $(filter-out ../nvn/nvn_mtl.m ../nvn/nvn_entry.S ../nvn/nvn_thunks.S,$(GFX)) $(FW)
# 依赖一行一个、按字母序（免得加一个测试就整行冲突）；下面各条命令的先后不变
# Dependencies one per line in alphabetical order (so that adding one test does not conflict on the whole line); the order of the commands below is unchanged
run: \
	t_adaptive_pace \
	t_bc8 \
	t_completion_wait \
	t_copy_pitch \
	t_builtin \
	t_cover \
	t_defer_chain \
	t_deswz_gpu \
	t_dirty_race \
	t_dirty_slots \
	t_fuse_check \
	t_hmap \
	t_mip_extent \
	t_nvn \
	t_pace \
	t_pfv \
	t_present_crop \
	t_pso_skey \
	t_refl \
	t_reorder_chain \
	t_rt_depth_ro \
	t_rt_range \
	t_rt_range_ro \
	t_rt_range_shape \
	t_rt_srgb_view \
	t_ub \
	t_xv \
	t_zskip
	./t_copy_pitch
	./t_nvn
	./t_dirty_slots
	./t_dirty_race
	./t_builtin
	./t_deswz_gpu
	./t_pace
	./t_adaptive_pace
	./t_bc8
	./t_completion_wait
	./t_refl
	./t_hmap
	./t_cover
	./t_fuse_check
	./t_present_crop
	MTL_DEBUG_LAYER=1 ./t_reorder_chain
	MTL_DEBUG_LAYER=1 ./t_defer_chain
	./t_zskip
	./t_xv
	./t_ub
	MTL_DEBUG_LAYER=1 ./t_mip_extent
	MTL_DEBUG_LAYER=1 ./t_mip_extent --stencil-only
	./t_rt_depth_ro
	./t_rt_range_shape
	./t_rt_range_ro
	DM_NVN_UNWRITTEN_DC=0 MTL_DEBUG_LAYER=1 ./t_rt_range
	MTL_DEBUG_LAYER=1 ./t_rt_range   # DontCare 缺省开：附件字节按「每通道省一次深度模板写回」闭式期望 | DontCare on by default: attachment bytes use the closed-form expectation of "one depth-stencil store saved per pass"
	DM_NVN_UNWRITTEN_DC=0 MTL_DEBUG_LAYER=1 ./t_rt_range --disabled
	DM_NVN_UNWRITTEN_DC=0 MTL_DEBUG_LAYER=1 ./t_rt_range --small-depth
	DM_NVN_UNWRITTEN_DC=0 MTL_DEBUG_LAYER=1 ./t_rt_range --sample-attachment --both-flags
	./t_pso_skey
	MTL_DEBUG_LAYER=1 ./t_pfv
	MTL_DEBUG_LAYER=1 ./t_rt_srgb_view
	python3 test_tbdr_budget.py
.PHONY: run
# 旧接口版那一遍（图形后端编译期开关 DM_NVN_M3=1，目标 macOS 15.0）：在旁边的 tests_m3 目录里用同一份测试源码另编另跑，不动这里的缺省产物。
# 闸门（verify/accept.sh）两遍都跑，免得以后只测一边
# The older-API pass (compile-time switch DM_NVN_M3=1 of the graphics backend, target macOS 15.0): built and run separately from the same test sources in the sibling tests_m3 directory, leaving the default outputs here untouched.
# The gate (verify/accept.sh) runs both passes, so that only one side being tested cannot happen later
run-m3:
	rm -rf ../tests_m3 && mkdir ../tests_m3 && cp Makefile *.c *.cpp *.m *.py ../tests_m3/
	$(MAKE) -C ../tests_m3 run CFLAGS="$(CFLAGS) -DDM_NVN_M3=1 -mmacosx-version-min=15.0"

t_copy_pitch: t_copy_pitch.c ../nvn/nvn_copy_pitch.h
	clang -O1 -Wall -Werror -o $@ t_copy_pitch.c
