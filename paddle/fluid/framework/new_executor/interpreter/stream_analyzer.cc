// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/framework/new_executor/interpreter/stream_analyzer.h"

#include <future>
#include <unordered_set>

#include "paddle/fluid/framework/new_executor/interpreter/interpreter_util.h"
#include "paddle/fluid/platform/collective_helper.h"
#include "paddle/fluid/platform/device_context.h"

namespace paddle {
namespace framework {
namespace interpreter {

using DeviceContext = platform::DeviceContext;
using DeviceEvent = platform::DeviceEvent;

class ContextManager {
 public:
  using DeviceContextMap =
      std::map<Place, std::shared_future<std::unique_ptr<DeviceContext>>>;

  static ContextManager& Instance() {
    static ContextManager* ctx_manager = new ContextManager;
    return *ctx_manager;
  }

  std::shared_future<std::unique_ptr<DeviceContext>> Get(
      const std::string& type,
      const platform::Place& place,
      int stream_priority) {
    std::lock_guard<std::mutex> lk(ctx_mtx_);
    VLOG(6) << "Get dev_ctx for " << type << " - " << place;

    DeviceContextMap& ctxs = ctx_pool_[type];
    if (ctxs.find(place) == ctxs.end()) {
      platform::EmplaceDeviceContexts(
          &ctxs,
          {place},
          /*disable_setting_default_stream_for_allocator=*/true,
          stream_priority);
    }
    return ctxs[place];
  }

 private:
  ContextManager() {}
  DISABLE_COPY_AND_ASSIGN(ContextManager);

  std::mutex ctx_mtx_;
  std::unordered_map<std::string, DeviceContextMap> ctx_pool_;
};

inline std::string RunTypeToString(DownstreamRunType run_type) {
  if (run_type == DownstreamRunType::kDirectRun) {
    return "DirectRun";
  } else {
    return "EventRun";
  }
}

void StreamAnalyzer::ConstructEvents(
    std::vector<Instruction>* instructions) const {
  std::vector<Instruction> cross_step_merged_instructions = *instructions;
  for (const Instruction& instr : *instructions) {
    cross_step_merged_instructions.emplace_back(instr);
  }

  DependencyBuilder dependency_builder;
  dependency_builder.Build(cross_step_merged_instructions);

  const std::map<size_t, std::set<size_t>>& downstream_map =
      dependency_builder.OpDownstreamMap();
  const size_t instr_num = cross_step_merged_instructions.size();
  std::vector<std::vector<std::vector<size_t>>> run_type_info(
      instr_num,
      std::vector<std::vector<size_t>>(
          /*number_of_run_type = */ 2));  // instr_id -> run_type ->
                                          // next_instr_id
  AnalyseAllRunType(
      cross_step_merged_instructions, downstream_map, &run_type_info);

  std::map<const DeviceContext*, std::map<size_t, std::set<size_t>>>
      event_info;  // DeviceContext -> waiter_instr_id -> recorder_instr_ids
  AnalyseAllEventInfo(
      cross_step_merged_instructions, run_type_info, &event_info);
  ShrinkEventInfo(dependency_builder, &event_info);

  // Construct events
  std::map<size_t, std::shared_ptr<DeviceEvent>> instr2event;
  for (auto& context_item : event_info) {
    for (auto& waiter_item : context_item.second) {
      size_t waiter_instr_id = waiter_item.first;
      std::set<size_t>& recorder_instr_ids = waiter_item.second;

      if (waiter_instr_id >= instructions->size()) {
        waiter_instr_id -= instructions->size();
      }

      for (size_t recorder_instr_id : recorder_instr_ids) {
        // Redundant record
        if (recorder_instr_id >= instructions->size()) {
          continue;
        }

        Instruction& recorder_instr = instructions->at(recorder_instr_id);
        Instruction& waiter_instr = instructions->at(waiter_instr_id);
        platform::DeviceType waiter_type = GetWaiterType(waiter_instr);

        if (instr2event.find(recorder_instr_id) == instr2event.end()) {
          std::shared_ptr<DeviceEvent> device_event =
              std::make_shared<DeviceEvent>(
                  recorder_instr.DeviceContext().GetPlace(),
                  platform::GenerateDeviceEventFlag());
          recorder_instr.AddEventToRecord(device_event,
                                          platform::kCUDA /*unused*/);
          instr2event.emplace(recorder_instr_id, device_event);
        }

        waiter_instr.AddEventToWait(
            recorder_instr_id, instr2event.at(recorder_instr_id), waiter_type);
        VLOG(6) << "Add event: " << recorder_instr.OpBase()->Type() << "("
                << recorder_instr_id << ") -> " << waiter_instr.OpBase()->Type()
                << "(" << waiter_instr_id << "), waiter type = " << waiter_type;
      }
    }
  }
}

DeviceContext* StreamAnalyzer::ParseDeviceContext(
    const OpFuncNode& op_func_node) const {
  auto& op = op_func_node.operator_base_;
  auto& op_type = op->Type();
  const std::string& execution_stream = op_func_node.execution_stream_;
  const int stream_priority = op_func_node.stream_priority_;
  ContextManager& ctx_manager = ContextManager::Instance();

  // only gpu/npu need update. xpu not need, because xpu memcpy op kernel is
  // synchronous.
  if (platform::is_gpu_place(place_) || platform::is_npu_place(place_) ||
      platform::is_custom_place(place_)) {
    VLOG(6) << "Parse DeviceContext for " << op_type
            << ", execution stream = " << execution_stream;
    if (execution_stream != kDefaultStream) {
      return ctx_manager
          .Get(std::string(kCustomStream) + "-" + execution_stream,
               place_,
               stream_priority)
          .get()
          .get();
    }

    if (op_type == interpreter::kMemcpyD2H) {
      return ctx_manager.Get(std::string(kD2HStream), place_, stream_priority)
          .get()
          .get();
    } else if (op_type == interpreter::kMemcpyH2D) {
      return ctx_manager.Get(std::string(kH2DStream), place_, stream_priority)
          .get()
          .get();
    }

#if defined(PADDLE_WITH_NCCL) || defined(PADDLE_WITH_RCCL)
    // NOTE(Ruibiao): Here supports multi-stream overlap for c_allreduce_sum
    // with use_cal_stream==false by returning a device context getting from the
    // global NCCLCommContext instance. Because when use_calc_stream==false, in
    // OP kernel, the NCCL communication will be launched to the stream directly
    // getting from the global NCCLCommContext instance rather than the
    // DeviceContext passed from executor (see CAllReduceOpCUDAKernel in
    // c_allreduce_op.h). Now it is just a temporary solution for ONLY
    // c_allreduce_sum which is used in ResNet50 distributed training.
    if (op_type == "c_allreduce_sum" &&
        op->Attr<bool>("use_calc_stream") == false) {
      int ring_id = op->Attr<int>("ring_id");
      return platform::NCCLCommContext::Instance()
          .Get(ring_id, place_)
          ->dev_context();
    }
#endif
  }

  return op_func_node.dev_ctx_;
}

bool StreamAnalyzer::HasDataDependency(const Instruction& cur_instr,
                                       const Instruction& next_instr) const {
  auto no_need_buffer_ins =
      [](const Instruction& instr) -> const std::unordered_set<std::string> {
    auto* op = instr.OpBase();
    auto& inferer = op->Info().NoNeedBufferVarsInferer();
    if (inferer) {
      return inferer(op->Inputs(), op->Outputs(), op->Attrs());
    }
    return std::unordered_set<std::string>();
  };

  // cur_instr->var->next_instr
  std::unordered_set<size_t> cur_var_ids;
  for (auto& item : cur_instr.Outputs()) {
    cur_var_ids.insert(item.second.begin(), item.second.end());
  }

  const std::unordered_set<std::string> next_instr_no_need_buffer_ins =
      no_need_buffer_ins(next_instr);

  for (auto& item : next_instr.Inputs()) {
    if (next_instr_no_need_buffer_ins.find(item.first) !=
        next_instr_no_need_buffer_ins.end()) {
      continue;
    }
    for (auto next_var_id : item.second) {
      if (cur_var_ids.find(next_var_id) != cur_var_ids.end()) {
        VLOG(6) << "Found data dependency from " << cur_instr.OpBase()->Type()
                << "(" << cur_instr.Id() << ") to "
                << next_instr.OpBase()->Type() << "(" << next_instr.Id()
                << ") at variable " << item.first << "(" << next_var_id << ")";
        return true;
      }
    }
  }

  // cur_instr->var && next_instr->var
  // var->cur_instr && next_instr->var
  const std::unordered_set<std::string> cur_instr_no_need_buffer_ins =
      no_need_buffer_ins(cur_instr);
  for (auto& item : cur_instr.Inputs()) {
    if (cur_instr_no_need_buffer_ins.find(item.first) ==
        cur_instr_no_need_buffer_ins.end()) {
      cur_var_ids.insert(item.second.begin(), item.second.end());
    }
  }

  for (auto& item : next_instr.Outputs()) {
    for (auto next_var_id : item.second) {
      if (cur_var_ids.find(next_var_id) != cur_var_ids.end()) {
        VLOG(6) << "Found data dependency from " << cur_instr.OpBase()->Type()
                << "(" << cur_instr.Id() << ") to "
                << next_instr.OpBase()->Type() << "(" << next_instr.Id()
                << ") at variable " << item.first << "(" << next_var_id << ")";
        return true;
      }
    }
  }

  return false;
}

void StreamAnalyzer::AnalyseAllEventInfo(
    const std::vector<Instruction>& instructions,
    const std::vector<std::vector<std::vector<size_t>>>& run_type_info,
    std::map<const DeviceContext*, std::map<size_t, std::set<size_t>>>*
        event_info) const {
  for (size_t cur_instr_id = 0; cur_instr_id < instructions.size();
       ++cur_instr_id) {
    const std::vector<size_t>& next_instr_ids =
        run_type_info[cur_instr_id][DownstreamRunType::kEventRun];
    std::set<size_t> waiter_instr_ids;
    std::set<size_t> visited_next_instr_id;

    for (size_t next_instr_id : next_instr_ids) {
      AnalyseEventInfoForTwoInstructions(instructions,
                                         run_type_info,
                                         cur_instr_id,
                                         next_instr_id,
                                         &waiter_instr_ids,
                                         &visited_next_instr_id);
    }

    for (size_t waiter_instr_id : waiter_instr_ids) {
      (*event_info)[&(instructions[cur_instr_id].DeviceContext())]
                   [waiter_instr_id]
                       .insert(cur_instr_id);
    }
  }
}

void StreamAnalyzer::AnalyseAllRunType(
    const std::vector<Instruction>& instructions,
    const std::map<size_t, std::set<size_t>>& downstream_map,
    std::vector<std::vector<std::vector<size_t>>>* run_type_info) const {
  for (auto& item : downstream_map) {
    size_t cur_instr_id = item.first;
    const Instruction& cur_instr = instructions[item.first];
    for (size_t next_instr_id : item.second) {
      const Instruction& next_instr = instructions[next_instr_id];
      DownstreamRunType run_type =
          AnalyseRunTypeForTwoInstructions(cur_instr, next_instr);

      (*run_type_info)[cur_instr_id][run_type].push_back(next_instr_id);

      VLOG(6) << RunTypeToString(run_type) << ": " << cur_instr.OpBase()->Type()
              << "(" << cur_instr_id << ") -> " << next_instr.OpBase()->Type()
              << "(" << next_instr_id << ")";
    }
  }
}

// The caller should guarantee cur_instr and next_instr is kEventRun
void StreamAnalyzer::AnalyseEventInfoForTwoInstructions(
    const std::vector<Instruction>& instructions,
    const std::vector<std::vector<std::vector<size_t>>>& run_type_info,
    const size_t cur_instr_id,
    const size_t next_instr_id,
    std::set<size_t>* waiter_instr_ids,
    std::set<size_t>* visited_next_instr_id) const {
  if (visited_next_instr_id->find(next_instr_id) !=
      visited_next_instr_id->end()) {
    return;
  }
  visited_next_instr_id->insert(next_instr_id);

  // NOTE(Ruibiao): Though depend_op as next_instr is no_need_buffer, we should
  // also wait event for it. Because depend_op is used to build dependencies for
  // fused vars in some scenarios. In those cases, we do not know which vars may
  // lead a implicit data dependency. For example,
  // ###
  // ### fused_var = fuse_op(var0, ...)
  // ### var1 = op1(fused_var)
  // ### var0 = depend_op(var0, fused_var)
  // ### var2 = op2(var0)
  // ###
  // If op1 are cross-stream with depend_op and op2, then we have:
  // ###
  // ### event_run : op1 -> depend_op
  // ### direct_run : depend_op -> op2
  // ###
  // There is actually a data dependency between op1 and op2 that var0 and
  // fused_var share the same tensor. However, as the dependency is implicit, we
  // can only add event for it with the help of depend_op.
  if (HasDataDependency(instructions[cur_instr_id],
                        instructions[next_instr_id]) ||
      run_type_info[next_instr_id][DownstreamRunType::kEventRun].size() ||
      instructions[next_instr_id].OpBase()->Type() == "depend") {
    waiter_instr_ids->insert(next_instr_id);
    return;
  }

  // NOTE(Ruibiao): If no data dependency from cur_instr to next_instr, and
  // simultaneously next_instr has no event_run downstream instr, we try to
  // recursively add events between cur_instr and next_instr's
  // direct-run-instrs. This can delay the event wait and achieve better
  // scheduling performance in some scenarios. However, when next_instr has too
  // many direct-run-instrs, it may perform worse than add event directly
  // between cur_instr and next_instr.
  for (size_t instr_id :
       run_type_info[next_instr_id][DownstreamRunType::kDirectRun]) {
    AnalyseEventInfoForTwoInstructions(instructions,
                                       run_type_info,
                                       cur_instr_id,
                                       instr_id,
                                       waiter_instr_ids,
                                       visited_next_instr_id);
  }
}

void StreamAnalyzer::ShrinkEventInfo(
    const DependencyBuilder& dependency_builder,
    std::map<const DeviceContext*, std::map<size_t, std::set<size_t>>>*
        event_info) const {
  for (auto& item : *event_info) {
    // shrink redundant recorders, waiter instrs should only wait for the last
    // recorder instrs in each stream
    std::map<size_t, std::set<size_t>>& waiter_recorder_map = item.second;
    for (auto& waiter_recorder : waiter_recorder_map) {
      size_t waiter_instr_id = waiter_recorder.first;
      std::set<size_t>& recorder_instr_ids = waiter_recorder.second;
      std::set<size_t> unnecessary_recorder_instr_ids;
      for (size_t cur_instr_id : recorder_instr_ids) {
        for (size_t next_instr_id : recorder_instr_ids) {
          if (dependency_builder.OpHappensBefore(cur_instr_id, next_instr_id)) {
            unnecessary_recorder_instr_ids.insert(cur_instr_id);
            break;
          }
        }
      }

      for (size_t unnecessary_recorder_instr_id :
           unnecessary_recorder_instr_ids) {
        VLOG(8) << "Shrink event : " << unnecessary_recorder_instr_id << " -> "
                << waiter_instr_id;
        recorder_instr_ids.erase(unnecessary_recorder_instr_id);
      }
    }

    // shrink redundant waiters, recorder instrs should only wait by the first
    // waiter instrs in each stream
    std::map<size_t, std::set<size_t>> recorder_waiter_map;
    for (auto& waiter_recorder : waiter_recorder_map) {
      size_t waiter_instr_id = waiter_recorder.first;
      std::set<size_t>& recorder_instr_ids = waiter_recorder.second;
      for (size_t record_instr_id : recorder_instr_ids) {
        recorder_waiter_map[record_instr_id].insert(waiter_instr_id);
      }
    }

    for (auto& recorder_waiter : recorder_waiter_map) {
      size_t recorder_instr_id = recorder_waiter.first;
      std::set<size_t>& waiter_instr_ids = recorder_waiter.second;
      std::set<size_t> unnecessary_waiter_instr_ids;
      for (size_t cur_instr_id : waiter_instr_ids) {
        for (size_t next_instr_id : waiter_instr_ids) {
          if (dependency_builder.OpHappensBefore(cur_instr_id, next_instr_id)) {
            unnecessary_waiter_instr_ids.insert(next_instr_id);
            break;
          }
        }
      }

      for (size_t unnecessary_wiater_instr_id : unnecessary_waiter_instr_ids) {
        VLOG(8) << "Shrink event : " << recorder_instr_id << " -> "
                << unnecessary_wiater_instr_id;
        waiter_recorder_map[unnecessary_wiater_instr_id].erase(
            recorder_instr_id);
      }
    }
  }
}

platform::DeviceType StreamAnalyzer::GetWaiterType(
    const Instruction& instr) const {
  if (instr.KernelType() == OpFuncType::kCpuSync ||
      instr.KernelType() == OpFuncType::kGpuSync) {
    return platform::kCPU;
  } else {
    if (platform::is_xpu_place(place_)) {
      return platform::kXPU;
    } else if (platform::is_npu_place(place_)) {
      return platform::kNPU;
    } else if (platform::is_custom_place(place_)) {
      return platform::kCUSTOM_DEVICE;
    }
    return platform::kCUDA;
  }
}

DownstreamRunType StreamAnalyzer::AnalyseRunTypeForTwoInstructions(
    const Instruction& cur_instr, const Instruction& next_instr) const {
  // xpu&ipu memcpy kerenl is synchronous.
  if (platform::is_ipu_place(place_) || platform::is_xpu_place(place_)) {
    return DownstreamRunType::kDirectRun;
  }

  // npu d2h kernel is asynchronous.
  if (platform::is_npu_place(place_) || platform::is_custom_place(place_)) {
    if (interpreter::IsCpuOp(cur_instr) ||
        interpreter::IsMemcpyH2D(next_instr)) {
      return DownstreamRunType::kDirectRun;
    }
  }

  if (cur_instr.KernelType() == OpFuncType::kGpuAsync &&
      (&cur_instr.DeviceContext() != &next_instr.DeviceContext())) {
    return DownstreamRunType::kEventRun;
  }

  return DownstreamRunType::kDirectRun;
}

}  // namespace interpreter
}  // namespace framework
}  // namespace paddle
