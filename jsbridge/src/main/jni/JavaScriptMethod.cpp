/*
 * Copyright (C) 2019 ProSiebenSat1.Digital GmbH.
 *
 * Originally based on Duktape Android:
 * Copyright (C) 2015 Square, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "JavaScriptMethod.h"

#include "AutoReleasedJSValue.h"
#include "ExceptionHandler.h"
#include "JavaType.h"
#include "JniCache.h"
#include "JniTypes.h"
#include "JsBridgeContext.h"
#include "exceptions/JsException.h"
#include "jni-helpers/JniContext.h"
#include "jni-helpers/JObjectArrayLocalRef.h"
#include <stdexcept>
#include <string>

JavaScriptMethod::JavaScriptMethod(const JsBridgeContext *jsBridgeContext, const JniRef<jsBridgeMethod> &method, std::string methodName, bool isLambda)
 : m_methodName(std::move(methodName))
 , m_isLambda(isLambda) {

  const JniCache *jniCache = jsBridgeContext->getJniCache();
  const JavaTypeProvider &javaTypeProvider = jsBridgeContext->getJavaTypeProvider();

  MethodInterface methodInterface = jniCache->getMethodInterface(method);
  m_isVarArgs = methodInterface.isVarArgs();

  {
    // Create return value loader
    JniLocalRef<jsBridgeParameter> returnParameter = methodInterface.getReturnParameter();
    m_returnValueType = std::move(javaTypeProvider.makeUniqueType(returnParameter, true /*boxed*/));
    m_returnValueParameter = JniGlobalRef<jsBridgeParameter>(returnParameter);
  }

  JObjectArrayLocalRef parameters = methodInterface.getParameters();
  const auto numParameters = (size_t) parameters.getLength();

  // Release any local objects allocated in this frame when we leave this scope.
  //const JniLocalFrame localFrame(jniContext, numArgs);

  m_argumentTypes.resize(numParameters);

  // Create JavaType instances
  for (jsize i = 0; i < numParameters; ++i) {
    JniLocalRef<jsBridgeParameter> parameter = parameters.getElement<jsBridgeParameter>(i);

    if (m_isVarArgs && i == numParameters - 1) {
        ParameterInterface parameterInterface = jsBridgeContext->getJniCache()->getParameterInterface(parameter);
        JniLocalRef<jsBridgeParameter> varArgParameter = parameterInterface.getGenericParameter();
        auto javaType = jsBridgeContext->getJavaTypeProvider().makeUniqueType(varArgParameter, false /*boxed*/);
        m_argumentTypes[i] = std::move(javaType);
        break;
    }

    // Always load the boxed type instead of the primitive type (e.g. Integer vs int)
    // because we are going to a Proxy object
    auto javaType = javaTypeProvider.makeUniqueType(parameter, true /*boxed*/);
    m_argumentTypes[i] = std::move(javaType);
  }
}

JavaScriptMethod::JavaScriptMethod(JavaScriptMethod &&other) noexcept
 : m_methodName(std::move(other.m_methodName))
 , m_returnValueType(std::move(other.m_returnValueType))
 , m_returnValueParameter(std::move(other.m_returnValueParameter))
 , m_argumentTypes(std::move(other.m_argumentTypes))
 , m_isLambda(other.m_isLambda)
 , m_isVarArgs(other.m_isVarArgs) {
}

JavaScriptMethod &JavaScriptMethod::operator=(JavaScriptMethod &&other) noexcept {
  m_methodName = std::move(other.m_methodName);
  m_returnValueType = std::move(other.m_returnValueType);
  m_returnValueParameter = std::move(other.m_returnValueParameter);
  m_argumentTypes = std::move(other.m_argumentTypes);
  m_isLambda = other.m_isLambda;
  m_isVarArgs = other.m_isVarArgs;

  return *this;
}

#if defined(DUKTAPE)

#include "StackChecker.h"

JValue JavaScriptMethod::invoke(const JsBridgeContext *jsBridgeContext, void *jsHeapPtr, const JObjectArrayLocalRef &args, bool awaitJsPromise) const {
  duk_context *ctx = jsBridgeContext->getDuktapeContext();
  CHECK_STACK(ctx);

  JValue result;

  // Set up the call - push the object, method name, and arguments onto the stack
  duk_push_heapptr(ctx, jsHeapPtr);
  duk_idx_t jsLambdaOrObjectIdx = duk_normalize_index(ctx, -1);

  if (m_isLambda) {
    duk_require_function(ctx, jsLambdaOrObjectIdx);
  } else {
    duk_require_object(ctx, jsLambdaOrObjectIdx);
    duk_push_string(ctx, m_methodName.c_str());
  }

  jsize numArguments = args.isNull() ? 0U : args.getLength();

  for (jsize i = 0; i < numArguments; ++i) {
    JValue arg(args.getElement(i));
    const auto &argumentType = m_argumentTypes[i];
    try {
      if (m_isVarArgs && i == numArguments - 1) {
        numArguments = i + argumentType->pushArray(arg.getLocalRef().staticCast<jarray>(), true /*expand*/);
        break;
      }
      argumentType->push(arg);
    } catch (const std::exception &) {
      duk_pop_n(ctx, (m_isLambda ? 1 : 2) + i);  // lambda: func + previous args, method: obj + methodName + previous args
      throw;
    }
  }

  duk_ret_t ret;
  if (m_isLambda) {
    ret = duk_pcall(ctx, numArguments);  // [... func arg1 ... argN] -> [... retval]
  } else {
    ret = duk_pcall_prop(ctx, jsLambdaOrObjectIdx, numArguments);  // [... obj ... key arg1 ... argN] -> [... obj ... retval]
    duk_remove(ctx, jsLambdaOrObjectIdx);
  }
  if (ret == DUK_EXEC_SUCCESS) {
    try {
      bool isDeferred = awaitJsPromise && duk_is_object(ctx, -1) && duk_has_prop_string(ctx, -1, "then");
      if (isDeferred && !m_returnValueType->isDeferred()) {
        result = jsBridgeContext->getJavaTypeProvider().getDeferredType(m_returnValueParameter)->pop();
      } else {
        result = m_returnValueType->pop();
      }
    } catch (const std::exception &) {
      throw;
    }
  } else {
    throw jsBridgeContext->getExceptionHandler()->getCurrentJsException();
  }

  return result;
};

#elif defined(QUICKJS)

#include "QuickJsUtils.h"

JValue JavaScriptMethod::invoke(const JsBridgeContext *jsBridgeContext, JSValueConst jsMethod, JSValueConst jsThis, const JObjectArrayLocalRef &javaArgs, bool awaitJsPromise) const {
  JSContext *ctx = jsBridgeContext->getQuickJsContext();

  int numJavaArguments = javaArgs.isNull() ? 0 : (int) javaArgs.getLength();
  int numJsArguments = numJavaArguments;

  JniLocalRef<jarray> varArgJavaArray;
  int varArgCount = 0;

  if (m_isVarArgs) {
    varArgJavaArray = javaArgs.getElement(numJavaArguments - 1).staticCast<jarray>();
    varArgCount = jsBridgeContext->getJniContext()->getArrayLength(varArgJavaArray);
    numJsArguments += varArgCount - 1;
  }

  JSValue jsArgs[numJsArguments];

  for (jsize i = 0; i < numJsArguments; ++i) {
    JValue javaArg(javaArgs.getElement(i));
    const auto &argumentType = m_argumentTypes[i];
    try {
      if (m_isVarArgs && i >= numJavaArguments - 1) {
        // For varargs, convert Java array to JS array and "expand" it to the JS args
        JSValue varArgJsArray = argumentType->fromJavaArray(varArgJavaArray);
        for (int j = 0; j < varArgCount; ++j, ++i) {
          JSValue elementJsValue = JS_GetPropertyUint32(ctx, varArgJsArray, static_cast<uint32_t>(j));
          jsArgs[i] = elementJsValue;
        }
        JS_FreeValue(ctx, varArgJsArray);
        break;
      }
      jsArgs[i] = argumentType->fromJava(javaArg);
    } catch (const std::exception &) {
      // Free all the JSValue instances which had been added until now
      for (int j = 0; j < i; ++j) {
        JS_FreeValue(ctx, jsArgs[j]);
      }
      throw;
    }
  }

  JSValue ret = JS_Call(ctx, jsMethod, jsThis, numJsArguments, jsArgs);
  JS_AUTORELEASE_VALUE(ctx, ret);

  for (jsize i = 0; i < numJsArguments; ++i) {
    JS_FreeValue(ctx, jsArgs[i]);
  }

  if (JS_IsException(ret)) {
    throw jsBridgeContext->getExceptionHandler()->getCurrentJsException();
  }

  bool isDeferred = awaitJsPromise && JS_IsObject(ret) && jsBridgeContext->getUtils()->hasPropertyStr(ret, "then");
  if (isDeferred && !m_returnValueType->isDeferred()) {
    return jsBridgeContext->getJavaTypeProvider().getDeferredType(m_returnValueParameter)->toJava(ret);
  }

  return m_returnValueType->toJava(ret);
};

#endif
