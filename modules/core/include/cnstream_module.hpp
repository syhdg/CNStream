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

#ifndef CNSTREAM_MODULE_HPP_
#define CNSTREAM_MODULE_HPP_

/**
 * @file cnstream_module.hpp
 *
 * This file contains a declaration of the Module class and the ModuleFactoryclass.
 */
#include <cxxabi.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cnstream_common.hpp"
#include "cnstream_eventbus.hpp"
#include "cnstream_frame.hpp"

namespace cnstream {

using ModuleParamSet = std::unordered_map<std::string, std::string>;

/**
 * @brief Module virtual base class.
 *
 * Module is the parent class of all modules. A module could have configurable
 * number of upstream links and downstream links.
 * Some modules are already constructed with a framework,
 * such as decoder, inferencer, and so on.
 * You can also design your own modules.
 */
class Module {
 public:
  /**
   * Constructor.
   *
   * @param name The name of a module. Modules defined in a pipeline should
   *             have different names.
   */
  explicit Module(const std::string &name) : name_(name) { this->GetId(); }
  virtual ~Module() { this->ReturnId(); }

  /**
   * @deprecated
   *
   * Sets a module name.
   *
   * @param name The name of a module. Modules defined in a pipeline should
   *             have different names.
   *
   * @return Void.
   */
  void SetName(const std::string &name) { name_ = name; }

  /**
   * Opens resources for a module.
   *
   * @param param_set Parameters for this module.
   *
   * @return Returns true if this API run successfully. Otherwise, returns false.
   *
   * @note You do not need to call this function by yourself. This function will be called
   *       by pipeline when pipeline starts. The pipeline will call the Process function
   *       of this module automatically after the Open function is done.
   */
  virtual bool Open(ModuleParamSet param_set) = 0;

  /**
   * Closes resources for a module.
   *
   * @return Void.
   *
   * @note You do not need to call this function by yourself. This function will be called
   *       by pipeline when pipeline stops. The pipeline calls the Close function
   *       of this module automatically after the Open and Process functions are done.
   */
  virtual void Close() = 0;

  /**
   * Processes data.
   *
   * @param data The data that the module will process.
   *
   * @return
   * @retval 0 : OK, but framework needs to transmit data.
   * @retval 1: OK, the data has been handled by this module. The hasTransmit_ must be set).
   *            Module has to call Pipeline::ProvideData to tell pipeline to transmit data
   *            to next modules.
   * @retval >1: OK, data has been handled by this module, and pipeline will transmit data
   *             to next modules.
   * @retval <0: Pipeline will post an event with the EVENT_ERROR event type with return
   *             number.
   */
  virtual int Process(std::shared_ptr<CNFrameInfo> data) = 0;

  /**
   * Gets the name of this module.
   *
   * @return Returns the name of this module.
   */
  inline std::string GetName() const { return name_; }

  /**
   * Sets a container to this module and identifies which pipeline the module is added to.
   *
   * @note This function will be called automatically by the pipeline after this module
   *       is added into the pipeline. You do not need to call it by yourself.
   */
  inline void SetContainer(Pipeline *container) { container_ = container; }

  /**
   * Posts an event to the pipeline.
   *
   * @param type The type of an event.
   * @param msg Message string.
   *
   * @return Returns true if this function run successfully. Returns false if this
   *         module is not added to pipeline.
   */
  bool PostEvent(EventType type, const std::string &msg) const;

  /* useless for users */
  size_t GetId();
  /* useless for users */
  std::vector<size_t> GetParentIds() const { return parent_ids_; }
  /* useless for users, set upstream node id to this module */
  void SetParentId(size_t id) {
    parent_ids_.push_back(id);
    mask_ = 0;
    for (auto &v : parent_ids_) mask_ |= (uint64_t)1 << v;
  }

  /* useless for users */
  uint64_t GetModulesMask() const { return mask_; }

  /**
   * @return Returns whether this module has permission to transmit data by itself.
   *
   * @see Process
   */
  bool hasTranmit() const { return hasTransmit_.load(); }

 protected:
  const size_t INVALID_MODULE_ID = -1;
  Pipeline *container_ = nullptr;    ///< The container.
  std::string name_;                 ///< The name of the module.
  std::atomic<int> hasTransmit_{0};  ///< If it has permission to transmit data.

 private:
  void ReturnId();
  size_t id_ = -1;
  /* supports no more than 64 modules */
  static CNSpinLock module_id_spinlock_;
  static uint64_t module_id_mask_;

  std::vector<size_t> parent_ids_;
  uint64_t mask_ = 0;
};

class ModuleEx : public Module {
 public:
  explicit ModuleEx(const std::string &name) : Module(name) { hasTransmit_.store(1); }
};

/**
 * @brief ModuleCreator/ModuleFactory/ModuleCreatorWorker:
 *   Implements reflection mechanism to create a module instance dynamically with "ModuleClassName" and
 *   the "moduleName" parameter.
 *   Refer to the ActorFactory&DynamicCreator in https://github.com/Bwar/Nebula (under Apache2.0 license)
 */

/**
 * @brief ModuleFactory
 * Provides functions to create instances with "ModuleClassName" and the "moduleName" parameter.
 */
class ModuleFactory {
 public:
  static ModuleFactory *Instance() {
    if (nullptr == factory_) {
      factory_ = new ModuleFactory();
    }
    return (factory_);
  }
  virtual ~ModuleFactory(){};

  /**
   * Registers "ModuleClassName" and CreateFunction.
   *
   * @param strTypeName, ModuleClassName (TypeName).
   * @param pFunc, CreateFunction which has a parameter "moduleName".
   *
   * @return Returns true for success.
   */
  bool Regist(const std::string &strTypeName, std::function<Module *(const std::string &)> pFunc) {
    if (nullptr == pFunc) {
      return (false);
    }
    bool ret = map_.insert(std::make_pair(strTypeName, pFunc)).second;
    return ret;
  }

  /**
   * Creates a module instance with  "ModuleClassName" and "moduleName".
   *
   * @param strTypeName, ModuleClassName (TypeName).
   * @param name, The moduleName that is the parameter of CreateFunction.
   *
   * @return The module instance if run successfully. Returns nullptr if failed.
   */
  Module *Create(const std::string &strTypeName, const std::string &name) {
    auto iter = map_.find(strTypeName);
    if (iter == map_.end()) {
      return (nullptr);
    } else {
      return (iter->second(name));
    }
  }

 private:
  ModuleFactory(){};
  static ModuleFactory *factory_;
  std::unordered_map<std::string, std::function<Module *(const std::string &)> > map_;
};

/**
 * @brief ModuleCreator
 *   A concrete ModuleClass needs inherit ModuleCreator to enable reflection mechanism.
 *   ModuleCreator provides CreateFunction and registers ModuleClassName & CreateFunction to ModuleFactory.
 */
template <typename T>
class ModuleCreator {
 public:
  struct Register {
    Register() {
      char *szDemangleName = nullptr;
      std::string strTypeName;
#ifdef __GNUC__
      szDemangleName = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, nullptr);
#else
      // in this format?:     szDemangleName =  typeid(T).name();
      szDemangleName = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, nullptr);
#endif
      if (nullptr != szDemangleName) {
        strTypeName = szDemangleName;
        free(szDemangleName);
      }
      ModuleFactory::Instance()->Regist(strTypeName, CreateObject);
    }
    inline void do_nothing() const {};
  };
  ModuleCreator() { register_.do_nothing(); }
  virtual ~ModuleCreator() { register_.do_nothing(); };
  static T *CreateObject(const std::string &name) { return new T(name); }
  static Register register_;
};

template <typename T>
typename ModuleCreator<T>::Register ModuleCreator<T>::register_;

/**
 * @brief ModuleCreatorWorker, dynamic-creator helper.
 */
class ModuleCreatorWorker {
 public:
  Module *Create(const std::string &strTypeName, const std::string &name) {
    Module *p = ModuleFactory::Instance()->Create(strTypeName, name);
    return (p);
  }
};

}  // namespace cnstream

#endif  // CNSTREAM_MODULE_HPP_