/* Public-API C example: model container + schema-v2 enrollment array + one
 * sensitivity per keyword, streamed through <phonlet_wake.h>.
 * PCM input is headerless mono 16 kHz signed little-endian int16 on Linux x64.
 * Production applications should validate the versioned asset manifest before
 * use (the Node/Web/Python SDKs ship that validation). */
#include <phonlet_wake.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc,char** argv){
  if(argc<5){fprintf(stderr,"Usage: wake-c model.pwake keywords-array.json audio.s16le sensitivity [sensitivity ...]\n");return 2;}
  int status=1;pw_wake* wake=NULL;char* json=NULL;double* sensitivities=NULL;
  size_t k=(size_t)argc-4;int16_t pcm[512];int64_t offset=0;
  FILE* keywords=fopen(argv[2],"rb");FILE* audio=NULL;
  if(!keywords)goto cleanup;
  if(fseek(keywords,0,SEEK_END))goto cleanup;long bytes=ftell(keywords);if(bytes<1||bytes>1024*1024)goto cleanup;
  rewind(keywords);json=malloc((size_t)bytes+1);if(!json||fread(json,1,(size_t)bytes,keywords)!=(size_t)bytes)goto cleanup;json[bytes]=0;
  sensitivities=calloc(k,sizeof(double));if(!sensitivities)goto cleanup;
  for(size_t i=0;i<k;i++){char* end; sensitivities[i]=strtod(argv[i+4],&end);if(*end||end==argv[i+4])goto cleanup;}
  if(phonlet_wake_init(argv[1],json,sensitivities,(int32_t)k,&wake)!=PW_OK)goto cleanup;
  free(json);json=NULL;
  audio=fopen(argv[3],"rb");if(!audio)goto cleanup;
  size_t count;
  while((count=fread(pcm,1,sizeof(pcm),audio))>0){
    if(count%2)goto cleanup;count/=2;
    int32_t index=-1;
    if(phonlet_wake_process(wake,pcm,(int32_t)count,&index)!=PW_OK)goto cleanup;
    offset+=count/512; /* reads are frame-sized, so each call ends a frame */
    if(index>=0)printf("keyword=%d frame=%lld seconds=%.3f sensitivity=%.3f\n",index,(long long)offset,(offset)*.032,sensitivities[index]);
  }
  if(ferror(audio))goto cleanup;
  {int32_t tail=-1;if(phonlet_wake_finish(wake,&tail)!=PW_OK)goto cleanup;printf("frames=%lld discarded_samples=%d\n",(long long)offset,tail);}
  status=0;
cleanup:
  if(status)fprintf(stderr,"Wake error: %s (also check input paths/PCM format)\n",phonlet_wake_error());
  if(keywords)fclose(keywords);if(audio)fclose(audio);
  phonlet_wake_delete(wake);free(json);free(sensitivities);return status;
}
