#include <kws/runtime.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc,char** argv){
  try{
    if(argc<5)throw std::runtime_error("Usage: wake-cpp model.pwake keywords-array.json audio.s16le sensitivity [sensitivity ...]");
    std::ifstream input(argv[2]);if(!input)throw std::runtime_error("cannot read keyword array");
    std::string json((std::istreambuf_iterator<char>(input)),{});std::vector<double> sensitivity;
    for(int i=4;i<argc;i++){size_t used;double value=std::stod(argv[i],&used);if(argv[i][used])throw std::runtime_error("invalid sensitivity");sensitivity.push_back(value);}
    auto model=kws::ModelWeights::load(argv[1]);
    auto config=kws::parse_enrollments_per_keyword(json.c_str(),sensitivity.data(),sensitivity.size(),kws::KeywordMode::production_arbitrated,model->classes(),&model->language());
    kws::Session session(model,std::make_unique<kws::Detector>(std::move(config)));
    std::ifstream audio(argv[3],std::ios::binary);if(!audio)throw std::runtime_error("cannot read PCM input");
    std::array<int16_t,512> pcm;std::vector<uint8_t> fires(sensitivity.size());size_t offset=0;
    while(audio.read(reinterpret_cast<char*>(pcm.data()),sizeof(pcm))||audio.gcount()){
      if(audio.gcount()%2)throw std::runtime_error("partial PCM16 sample");
      kws::OutputBuffers out;out.capacity=1;out.fires=fires.data();
      auto frames=session.push_pcm(pcm.data(),audio.gcount()/2,out);
      for(size_t frame=0;frame<frames;frame++)for(size_t k=0;k<fires.size();k++)if(fires[k])std::cout<<"keyword="<<k<<" frame="<<offset+frame<<" sensitivity="<<sensitivity[k]<<'\n';
      offset+=frames;
    }
    if(audio.bad())throw std::runtime_error("PCM read failed");
    std::cout<<"frames="<<offset<<" discarded_samples="<<session.finish()<<'\n';
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
