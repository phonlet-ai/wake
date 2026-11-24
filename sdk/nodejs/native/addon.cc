// Node-API v8 only. Engine-owned scoring, one native Session per JS stream.
#include <node_api.h>
#include "kws/runtime.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>

namespace {
void ok(napi_status s) { if (s != napi_ok) throw std::runtime_error("invalid Node-API argument"); }
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Stream {
  void* session = nullptr;
  size_t keywords = 0, pending = 0, frames = 0;
  bool finished = false;
  ~Stream() { kws_session_destroy(session); }
};
const napi_type_tag stream_tag = {0x78c6b7a54882d414ULL, 0x90211c825a660db3ULL};
napi_value number(napi_env env, double x) { napi_value v; ok(napi_create_double(env,x,&v)); return v; }
void property(napi_env env,napi_value obj,const char* name,napi_value v) { ok(napi_set_named_property(env,obj,name,v)); }
std::string string(napi_env env,napi_value value) {
  size_t n; ok(napi_get_value_string_utf8(env,value,nullptr,0,&n));
  std::vector<char> data(n+1); ok(napi_get_value_string_utf8(env,value,data.data(),data.size(),&n)); return {data.data(),n};
}
void* typed(napi_env env,napi_value value,napi_typedarray_type expected,size_t& n) {
  napi_typedarray_type type; void* data; napi_value buffer; size_t offset;
  ok(napi_get_typedarray_info(env,value,&type,&n,&data,&buffer,&offset));
  require(type==expected,"wrong typed array type"); return data;
}
Stream& stream(napi_env env,napi_value value) {
  bool valid=false;ok(napi_check_object_type_tag(env,value,&stream_tag,&valid));require(valid,"invalid Wake stream");
  void* ptr;ok(napi_unwrap(env,value,&ptr));require(ptr,"missing stream");return *static_cast<Stream*>(ptr);
}
template<class F> napi_value invoke(napi_env env,F fn) {
  try { return fn(); } catch(const std::exception& e) { napi_throw_error(env,"ENGINE_FAILURE",e.what()); return nullptr; }
}
napi_value create(napi_env env,napi_callback_info info) {
  return invoke(env,[&] {
    size_t argc=4; napi_value args[4];ok(napi_get_cb_info(env,info,&argc,args,nullptr,nullptr));require(argc==4,"create needs four arguments");
    size_t bytes,k;auto* model=static_cast<uint8_t*>(typed(env,args[0],napi_uint8_array,bytes));
    auto json=string(env,args[1]);auto* sensitivities=static_cast<double*>(typed(env,args[2],napi_float64_array,k));
    int32_t mode;ok(napi_get_value_int32(env,args[3],&mode));
    std::unique_ptr<void,decltype(&kws_model_destroy)> weights(kws_model_load_bytes(model,bytes,"en"),kws_model_destroy);
    require(bool(weights),kws_error());
    auto s=std::make_unique<Stream>();s->keywords=k;
    s->session=kws_session_create_per_keyword(weights.get(),json.c_str(),sensitivities,k,mode);
    require(s->session,kws_error());
    napi_value result;ok(napi_create_object(env,&result));ok(napi_type_tag_object(env,result,&stream_tag));
    ok(napi_wrap(env,result,s.get(),[](napi_env,void* p,void*){delete static_cast<Stream*>(p);},nullptr,nullptr));
    s.release();return result;
  });
}
napi_value process(napi_env env,napi_callback_info info) {
  return invoke(env,[&] {
    size_t argc=2;napi_value args[2];ok(napi_get_cb_info(env,info,&argc,args,nullptr,nullptr));require(argc==2,"process needs two arguments");
    auto& s=stream(env,args[0]);require(s.session&&!s.finished,"stream is closed or finished");
    size_t n;auto* input=static_cast<int16_t*>(typed(env,args[1],napi_int16_array,n));
    require(n<=SIZE_MAX-s.pending,"input too large");size_t frames=(n+s.pending)/512;
    require(!frames||s.keywords<=SIZE_MAX/frames,"output too large");
    napi_value buffer,fires,result;void* output;
    ok(napi_create_arraybuffer(env,frames*s.keywords,&output,&buffer));
    auto count=kws_session_push(s.session,input,n,0,frames,nullptr,nullptr,nullptr,nullptr,static_cast<uint8_t*>(output),nullptr);
    require(count>=0,kws_error());
    ok(napi_create_typedarray(env,napi_uint8_array,frames*s.keywords,buffer,0,&fires));
    ok(napi_create_object(env,&result));property(env,result,"fires",fires);property(env,result,"frameOffset",number(env,s.frames));
    s.frames+=frames;s.pending=(s.pending+n)%512;return result;
  });
}
napi_value finish(napi_env env,napi_callback_info info) {
  return invoke(env,[&] {
    size_t argc=1;napi_value value;ok(napi_get_cb_info(env,info,&argc,&value,nullptr,nullptr));require(argc==1,"finish needs a stream");
    auto& s=stream(env,value);require(s.session&&!s.finished,"stream is closed or finished");
    auto discarded=kws_session_finish(s.session);require(discarded>=0,kws_error());s.finished=true;
    napi_value result;ok(napi_create_object(env,&result));property(env,result,"frames",number(env,s.frames));property(env,result,"discardedSamples",number(env,discarded));return result;
  });
}
napi_value close(napi_env env,napi_callback_info info) {
  return invoke(env,[&] {
    size_t argc=1;napi_value value;ok(napi_get_cb_info(env,info,&argc,&value,nullptr,nullptr));require(argc==1,"close needs a stream");
    auto& s=stream(env,value);kws_session_destroy(s.session);s.session=nullptr;napi_value result;ok(napi_get_undefined(env,&result));return result;
  });
}
napi_value init(napi_env env,napi_value exports) {
  for (auto entry : std::vector<std::pair<const char*,napi_callback>>{{"create",create},{"process",process},{"finish",finish},{"close",close}}) {
    napi_value fn;ok(napi_create_function(env,entry.first,NAPI_AUTO_LENGTH,entry.second,nullptr,&fn));property(env,exports,entry.first,fn);
  }
  property(env,exports,"abi",number(env,kws_abi_version()));property(env,exports,"frontendId",number(env,kws_frontend_id()));
  napi_value lang;ok(napi_create_string_utf8(env,kws_language("en"),NAPI_AUTO_LENGTH,&lang));property(env,exports,"language",lang);
  return exports;
}
}
NAPI_MODULE(wake_node,init)
