/*************************************************************************
 * Copyright (C) [2019] by Cambricon, Inc. All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *************************************************************************/

#include <easyinfer/mlu_context.h>
#include <easyinfer/model_loader.h>

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "infer_engine.hpp"
#include "infer_trans_data_helper.hpp"
#include "obj_filter.hpp"
#include "postproc.hpp"
#include "preproc.hpp"

#include "inferencer.hpp"
#include "perf_calculator.hpp"
#include "perf_manager.hpp"

namespace cnstream {

class PerfStats;

struct InferContext {
  std::shared_ptr<InferEngine> engine;
  std::shared_ptr<InferTransDataHelper> trans_data_helper;
  int drop_count = 0;
};  // struct InferContext

using InferContextSptr = std::shared_ptr<InferContext>;

class InferencerPrivate {
 public:
  explicit InferencerPrivate(Inferencer* q) : q_ptr_(q) {}
  std::shared_ptr<edk::ModelLoader> model_loader_;
  std::shared_ptr<Preproc> preproc_ = nullptr;
  std::shared_ptr<Postproc> postproc_ = nullptr;
  int device_id_ = 0;
  int interval_ = 0;
  uint32_t bsize_ = 1;
  float batching_timeout_ = 3000.0;  // ms
  std::map<std::thread::id, InferContextSptr> ctxs_;
  std::mutex ctx_mtx_;
  bool use_scaler_ = false;
  std::shared_ptr<PerfManager> infer_perf_manager_ = nullptr;
  std::thread cal_perf_th_;
  std::vector<std::string> th_ids_;
  std::atomic<bool> perf_th_running_{false};
  std::mutex th_ids_mutex;
  bool obj_infer_ = false;
  std::shared_ptr<ObjPreproc> obj_preproc_ = nullptr;
  std::shared_ptr<ObjPostproc> obj_postproc_ = nullptr;
  std::shared_ptr<ObjFilter> obj_filter_ = nullptr;
  bool keep_aspect_ratio_ = false;  // mlu preprocessing, keep aspect ratio

  void InferEngineErrorHnadleFunc(const std::string& err_msg) {
    LOG(FATAL) << err_msg;
    q_ptr_->PostEvent(EVENT_ERROR, err_msg);
  }

  InferContextSptr GetInferContext() {
    std::thread::id tid = std::this_thread::get_id();
    InferContextSptr ctx(nullptr);
    std::lock_guard<std::mutex> lk(ctx_mtx_);
    if (ctxs_.find(tid) != ctxs_.end()) {
      ctx = ctxs_[tid];
    } else {
      ctx = std::make_shared<InferContext>();
      std::stringstream ss;
      ss << tid;
      std::string tid_str = "th_" + ss.str();
      ctx->engine = std::make_shared<InferEngine>(
          device_id_, model_loader_, preproc_, postproc_, bsize_, batching_timeout_, use_scaler_,
          infer_perf_manager_, tid_str,
          std::bind(&InferencerPrivate::InferEngineErrorHnadleFunc, this, std::placeholders::_1),
          keep_aspect_ratio_,
          obj_infer_, obj_preproc_, obj_postproc_, obj_filter_);
      ctx->trans_data_helper = std::make_shared<InferTransDataHelper>(q_ptr_);
      ctxs_[tid] = ctx;
      if (infer_perf_manager_) {
        infer_perf_manager_->RegisterPerfType(tid_str, "pts", {"batching_done_time", "resize_start_time",
            "resize_end_time", "infer_start_time", "infer_end_time", "end_time"});
        infer_perf_manager_->CreatePerfCalculator(tid_str, "batching_done_time", "end_time");
        infer_perf_manager_->CreatePerfCalculator(tid_str, "resize_start_time", "resize_end_time");
        infer_perf_manager_->CreatePerfCalculator(tid_str, "infer_start_time", "infer_end_time");
        ctx->trans_data_helper->SetPerfManagerContext(infer_perf_manager_, tid_str);
        std::lock_guard<std::mutex> lg(th_ids_mutex);
        th_ids_.push_back(tid_str);
      }
    }
    return ctx;
  }

  void CalcPerf();
  void PrintPerf(std::string start, std::string end);

 private:
  DECLARE_PUBLIC(q_ptr_, Inferencer);
};  // class InferencerPrivate

void InferencerPrivate::PrintPerf(std::string start, std::string end) {
  PerfStats stats;
  double total_throughput = 0.f;
  for (auto it : th_ids_) {
    if (start == "batching_done_time" && end == "end_time") {
      stats = infer_perf_manager_->CalculateThroughput(it, start, end);
    } else {
      stats = infer_perf_manager_->CalculatePerfStats(it, start, end);
    }

    stats.frame_cnt *= bsize_;
    stats.fps *= bsize_;
    std::cout << it;
    if (start == "batching_done_time" && end == "end_time") {
      PrintThroughput(stats);
    } else {
      PrintPerfStats(stats);
    }
    total_throughput += stats.fps;
  }
  if (start == "batching_done_time" && end == "end_time") {
    std::cout << "Total Throughput: " << total_throughput << std::endl;
  }
}

void InferencerPrivate::CalcPerf() {
  while (perf_th_running_) {
    std::cout << "************************************** Inferencer Statistics ************************************\n";
    std::cout << "             (Average and Max Latency is for one batch. batch size is " << bsize_ << ")"<< std::endl;
    {
      std::lock_guard<std::mutex> lg(th_ids_mutex);
      std::cout << "---------- [resize and convert] ---------------------------------------------------------------\n";
      PrintPerf("resize_start_time", "resize_end_time");
      std::cout << "---------- [inference] ------------------------------------------------------------------------\n";
      PrintPerf("infer_start_time", "infer_end_time");
      std::cout << "---------- [inferencer module] ----------------------------------------------------------------\n";
      PrintPerf("batching_done_time", "end_time");
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

Inferencer::Inferencer(const std::string& name) : Module(name) {
  d_ptr_ = nullptr;
  hasTransmit_.store(1);  // transmit data by module itself
  param_register_.SetModuleDesc(
      "Inferencer is a module for running offline model inference,"
      " as well as preprocedding and postprocessing.");
  param_register_.Register("model_path",
                           "The offline model path. Normally offline model is a file"
                           " with cambricon extension.");
  param_register_.Register("func_name", "The offline model function name, usually is 'subnet0'.");
  param_register_.Register("peproc_name", "The preprocessing method name.");
  param_register_.Register("postproc_name", "The postprocessing method name.");
  param_register_.Register("device_id", "Which device will be used. If there is only one device, it might be 0.");
  param_register_.Register("batching_timeout",
                           "If we can not get a certain number (batch size) of frames"
                           " within a certain time (batching_timeout), we will stop waiting and append fake data.");
  param_register_.Register("data_order",
                           "It should be 'NCHW' (if the data is like, for example, rrr ggg bbb) or"
                           " 'NHWC' (e.g., rgb rgb rgb).");
  param_register_.Register("batch_size", "How many frames will be fed to model in one inference.");
  param_register_.Register("infer_interval",
                           "How many frames will be discarded between two frames"
                           " which will be fed to model for inference.");
  param_register_.Register("use_scaler", "Use scaler to do preprocess.");
  param_register_.Register("threshold", "The threshold of the results.");
  param_register_.Register("object_infer", "Whether to infer with frame or detection object.");
  param_register_.Register("obj_filter_name", "The object filter method name.");
  param_register_.Register("keep_aspect_ratio", "As the mlu is used for image processing, "
                            "the scale remains constant.");
}

Inferencer::~Inferencer() {}

bool Inferencer::Open(ModuleParamSet paramSet) {
  if (paramSet.find("model_path") == paramSet.end() || paramSet.find("func_name") == paramSet.end() ||
      paramSet.find("postproc_name") == paramSet.end()) {
    LOG(WARNING) << "Inferencer must specify [model_path], [func_name], [postproc_name].";
    return false;
  }

  if (d_ptr_) {
    Close();
  }
  d_ptr_ = new (std::nothrow) InferencerPrivate(this);
  if (!d_ptr_) {
    LOG(ERROR) << "Inferencer::Open() new InferencerPrivate failed";
    return false;
  }

  std::string model_path = paramSet["model_path"];
  model_path = GetPathRelativeToTheJSONFile(model_path, paramSet);

  std::string func_name = paramSet["func_name"];
  std::string Data_Order;
  if (paramSet.find("data_order") != paramSet.end()) {
    Data_Order = paramSet["data_order"];
  }

  try {
    d_ptr_->model_loader_ = std::make_shared<edk::ModelLoader>(model_path, func_name);

    if (Data_Order == "NCHW") {
      for (uint32_t index = 0; index < d_ptr_->model_loader_->OutputNum(); ++index) {
        edk::DataLayout layout;
        layout.dtype = edk::DataType::FLOAT32;
        layout.order = edk::DimOrder::NCHW;
        d_ptr_->model_loader_->SetCpuOutputLayout(layout, index);
      }
    }
    d_ptr_->model_loader_->InitLayout();
  } catch (edk::Exception& e) {
    LOG(ERROR) << "model path:" << model_path << ". " << e.what();
    return false;
  }

  d_ptr_->obj_infer_ = false;
  auto obj_infer_str = paramSet.find("object_infer");
  if (obj_infer_str != paramSet.end() && obj_infer_str->second == "true") {
    d_ptr_->obj_infer_ = true;
    LOG(INFO) << "[Inferencer] Inference mode: inference with object.";
    auto obj_filter_name_str = paramSet.find("obj_filter_name");
    if (obj_filter_name_str != paramSet.end()) {
      d_ptr_->obj_filter_ = std::shared_ptr<ObjFilter>(ObjFilter::Create(obj_filter_name_str->second));
      if (d_ptr_->obj_filter_) {
        LOG(INFO) << "[Inferencer] Object filter set:" << obj_filter_name_str->second;
      } else {
        LOG(ERROR) << "[Inferencer] Can not find ObjFilter implemention by name: " << obj_filter_name_str->second;
        return false;
      }
    }
  } else {
    LOG(INFO) << "[Inferencer] Inference mode: inference with frame.";
  }

  auto preproc_name = paramSet.find("preproc_name");
  if (preproc_name != paramSet.end()) {
    bool preproc_name_found = false;
    if (d_ptr_->obj_infer_) {
      d_ptr_->obj_preproc_ = std::shared_ptr<ObjPreproc>(ObjPreproc::Create(preproc_name->second));
      preproc_name_found = d_ptr_->obj_preproc_ != nullptr;
    } else {
      d_ptr_->preproc_ = std::shared_ptr<Preproc>(Preproc::Create(preproc_name->second));
      preproc_name_found = d_ptr_->preproc_ != nullptr;
    }
    if (!preproc_name_found) {
      LOG(ERROR) << "[Inferencer] CPU preproc name not found: " << preproc_name->second;
      return false;
    }
    LOG(INFO) << "[Inferencer] With CPU preproc set";
  }

  std::string postproc_name = paramSet["postproc_name"];
  bool postproc_name_found = false;
  if (d_ptr_->obj_infer_) {
    d_ptr_->obj_postproc_ = std::shared_ptr<cnstream::ObjPostproc>(cnstream::ObjPostproc::Create(postproc_name));
    postproc_name_found = d_ptr_->obj_postproc_ != nullptr;
  } else {
    d_ptr_->postproc_ = std::shared_ptr<cnstream::Postproc>(cnstream::Postproc::Create(postproc_name));
    postproc_name_found = d_ptr_->postproc_ != nullptr;
  }
  if (!postproc_name_found) {
    LOG(ERROR) << "[Inferencer] Can not find Postproc implemention by name: " << postproc_name;
    return false;
  }

  if (paramSet.find("keep_aspect_ratio") != paramSet.end() && paramSet["keep_aspect_ratio"] == "true") {
    LOG(INFO) << "[Inferencer] Keep aspect ratio has been set.";
    d_ptr_->keep_aspect_ratio_ = true;
  }

  if (paramSet.find("threshold") != paramSet.end()) {
    float threshold = std::stof(paramSet["threshold"]);
    if (d_ptr_->obj_infer_) {
      d_ptr_->obj_postproc_->SetThreshold(threshold);
    } else {
      d_ptr_->postproc_->SetThreshold(threshold);
    }
    LOG(INFO) << GetName() << " threshold: " << threshold;
  }

  d_ptr_->use_scaler_ = false;
  auto scaler_str = paramSet.find("use_scaler");
  if (scaler_str != paramSet.end() && scaler_str->second == "true") {
    d_ptr_->use_scaler_ = true;
  }

  if (paramSet.find("device_id") != paramSet.end()) {
    d_ptr_->device_id_ = std::stoi(paramSet["device_id"]);
  }

#ifdef CNS_MLU100
  if (paramSet.find("batch_size") != paramSet.end()) {
    std::stringstream ss;
    ss << paramSet["batch_size"];
    ss >> d_ptr_->bsize_;
  }
#elif CNS_MLU270
  d_ptr_->bsize_ = d_ptr_->model_loader_->InputShapes()[0].n;
#endif
  DLOG(INFO) << GetName() << " batch size:" << d_ptr_->bsize_;

  if (paramSet.find("infer_interval") != paramSet.end()) {
    d_ptr_->interval_ = std::stoi(paramSet["infer_interval"]);
    LOG(INFO) << GetName() << " infer_interval:" << d_ptr_->interval_;
  }

  // batching timeout
  if (paramSet.find("batching_timeout") != paramSet.end()) {
    d_ptr_->batching_timeout_ = std::stof(paramSet["batching_timeout"]);
    LOG(INFO) << GetName() << " batching timeout:" << d_ptr_->batching_timeout_;
  }

  if (paramSet.find("show_stats") != paramSet.end() && paramSet["show_stats"] == "true") {
    d_ptr_->infer_perf_manager_ = std::make_shared<PerfManager>();
    if (paramSet.find("stats_db_name") == paramSet.end()) {
        LOG(ERROR) << "Must set Inferencer custom parameter [stats_db_name] in config file.";
        return false;
    }
    if (!d_ptr_->infer_perf_manager_->Init(paramSet["stats_db_name"])) {
        LOG(ERROR) << "Init infer perf manager failed.";
        return false;
    }
    d_ptr_->perf_th_running_.store(true);
    d_ptr_->cal_perf_th_ = std::thread(&InferencerPrivate::CalcPerf, d_ptr_);
  }

  if (container_ == nullptr) {
    LOG(INFO) << name_ << " has not been added into pipeline.";
  } else {
  }

  /* hold this code. when all threads that set the cnrt device id exit, cnrt may release the memory itself */
  edk::MluContext ctx;
  ctx.SetDeviceId(d_ptr_->device_id_);
  ctx.ConfigureForThisThread();

  return true;
}

void Inferencer::Close() {
  if (nullptr == d_ptr_) return;

  d_ptr_->perf_th_running_.store(false);
  if (d_ptr_->cal_perf_th_.joinable()) d_ptr_->cal_perf_th_.join();

  /*destroy infer contexts*/
  d_ptr_->ctx_mtx_.lock();
  d_ptr_->ctxs_.clear();
  d_ptr_->ctx_mtx_.unlock();

  delete d_ptr_;
  d_ptr_ = nullptr;
}

int Inferencer::Process(CNFrameInfoPtr data) {
  std::shared_ptr<InferContext> pctx = d_ptr_->GetInferContext();
  bool eos = data->frame.flags & CNFrameFlag::CN_FRAME_FLAG_EOS;
  bool drop_data = d_ptr_->interval_ > 0 && pctx->drop_count++ % d_ptr_->interval_ != 0;

  if (!eos && data->frame.ctx.dev_id != d_ptr_->device_id_) {
    data->frame.CopyToSyncMemOnDevice(d_ptr_->device_id_);
  }

  if (eos || drop_data) {
    if (drop_data) pctx->drop_count %= d_ptr_->interval_;
    std::shared_ptr<std::promise<void>> promise = std::make_shared<std::promise<void>>();
    promise->set_value();
    InferEngine::ResultWaitingCard card(promise);
    pctx->trans_data_helper->SubmitData(std::make_pair(data, card));
  } else {
    InferEngine::ResultWaitingCard card = pctx->engine->FeedData(data);
    pctx->trans_data_helper->SubmitData(std::make_pair(data, card));
  }

  return 1;
}

bool Inferencer::CheckParamSet(const ModuleParamSet& paramSet) const {
  ParametersChecker checker;
  for (auto& it : paramSet) {
    if (!param_register_.IsRegisted(it.first)) {
      LOG(WARNING) << "[Inferencer] Unknown param: " << it.first;
    }
  }
  if (paramSet.find("model_path") == paramSet.end() || paramSet.find("func_name") == paramSet.end() ||
      paramSet.find("postproc_name") == paramSet.end()) {
    LOG(ERROR) << "Inferencer must specify [model_path], [func_name], [postproc_name].";
    return false;
  }
  if (!checker.CheckPath(paramSet.at("model_path"), paramSet)) {
    LOG(ERROR) << "[Inferencer] [model_path] : " << paramSet.at("model_path") << " non-existence.";
    return false;
  }
  std::string err_msg;
  if (!checker.IsNum({"batching_timeout", "device_id", "threshold"}, paramSet, err_msg)) {
    LOG(ERROR) << "[Inferencer] " << err_msg;
    return false;
  }
  return true;
}

}  // namespace cnstream
