/*
 * tools/dst_bench.c — верификация декодера DST (main/dst_dec.c)
 *
 * Сборка: gcc -O2 -o dst_bench dst_bench.c ../main/dst_dec.c -I../main -Ibench_stubs -lm
 * Использование:
 *   ./dst_bench file.dff [--dsd ref_dsd.bin] [--dump out.bin]   — прогон файла
 *   ./dst_bench --selftest                                     — встроенная проверка
 *
 * Бенч разбирает DFF (FRM8/DSD : FVER/PROP/DST), декодирует каждый DSTF
 * декодером проекта и по --dsd побайтово сверяет плотный интерлив-DSD
 * с эталоном (критерий корректности — DSD-биты; dsd2pcm даёт ±1 LSB
 * на границе полшага из-за FP-режима генератора эталона).
 *
 * Selftest строит синтетический DFF (несжатые DSTF) снизу вверх и сверяет
 * выход декодера с ожиданием по несжатой ветке: копия min(len-1, frame_bytes)
 * байтов данных + добивка 0x69 до frame_bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dst_dec.h"

static uint64_t be64(const uint8_t*p){uint64_t v=0;for(int i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static uint32_t be32(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint16_t be16(const uint8_t*p){return (uint16_t)((p[0]<<8)|p[1]);}
static void put_be64(uint8_t*p,uint64_t v){for(int i=7;i>=0;i--){p[i]=(uint8_t)(v&0xff);v>>=8;}}
static void put_be32(uint8_t*p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static void put_be16(uint8_t*p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}

static size_t next_chunk(const uint8_t*buf,size_t limit,size_t off,const char*id,size_t*plen)
{
    while (off+12<=limit) {
        if (!memcmp(buf+off,id,4)) { *plen=(size_t)be64(buf+off+4); return off+12; }
        size_t sz=(size_t)be64(buf+off+4);
        if (!sz||off+12+sz>limit) break;
        off+=12+sz;
    }
    return 0;
}

/* Разбор PROP: частота (BE64 в 'FS  ') и число каналов (BE16 в 'CHNL'). */
static int parse_prop(const uint8_t*buf,size_t limit,size_t po,size_t plen,
                      uint32_t*fs,int*channels)
{
    if (!po||!plen||!memcmp(buf+po,"SND ",4)==0) return -1;
    size_t so=po+4, sl;
    *fs=2822400; *channels=2;
    size_t fo=next_chunk(buf,po+plen,so,"FS  ",&sl);
    if (fo&&sl==8) *fs=(uint32_t)be64(buf+fo);
    size_t co=next_chunk(buf,po+plen,so,"CHNL",&sl);
    if (co&&sl>=2) *channels=be16(buf+co);
    return 0;
}

/* Полный проход: парс DFF + декодирование всех DSTF. */
static int dff_decode(const uint8_t*buf,size_t limit,
                      uint8_t**pout,size_t*ptotal,size_t*pok,size_t*pfail,
                      uint32_t*pfs,int*pch,size_t*pframes)
{
    if (limit<16||memcmp(buf,"FRM8",4)) return -1;
    if (memcmp(buf+12,"DSD ",4)) return -2;

    size_t plen; size_t po=next_chunk(buf,limit,16,"PROP",&plen);
    uint32_t fs=2822400; int channels=2;
    if (parse_prop(buf,limit,po,plen,&fs,&channels)) return -3;

    size_t dlen; size_t dpl=next_chunk(buf,limit,16,"DST ",&dlen);
    if (!dpl) return -4;
    size_t flen; size_t frte=next_chunk(buf,dpl+dlen,dpl,"FRTE",&flen);
    if (!frte||flen!=6) return -5;
    uint32_t nframes=be32(buf+frte);

    dst_handle_t h=dst_open(fs,(uint8_t)channels);
    if (!h) return -6;

    size_t spf=588u*(fs/44100);
    size_t frame_bytes=(spf/8)*(size_t)channels;
    /* Буфер на весь ожидаемый DSD-выход: frames*frame_bytes (+ запас на
       расхождение счётчика FRTE с фактическим числом DSTF). */
    size_t cap=frame_bytes*((size_t)nframes?nframes:1)+frame_bytes;
    uint8_t*out=malloc(cap);
    if (!out){dst_close(h);return -7;}

    size_t total=0,nok=0,nfail=0,off=frte+6;
    while (off+12<=limit) {
        if (memcmp(buf+off,"DSTF",4)) break;
        size_t sz=(size_t)be64(buf+off+4);
        if (!sz||off+12+sz>limit) break;
        if (total+frame_bytes>cap) break;
        esp_err_t rc=dst_decode_frame(h,buf+off+12,sz,out+total);
        if (rc==0){nok++;total+=frame_bytes;}
        else {nfail++;printf("frame %zu: rc=%d (skipped)\n",nok+nfail,(int)rc);}
        off+=12+sz;
    }
    dst_close(h);
    if (pout)*pout=out; else free(out);
    if (ptotal)*ptotal=total;
    if (pok)*pok=nok;
    if (pfail)*pfail=nfail;
    if (pfs)*pfs=fs;
    if (pch)*pch=channels;
    if (pframes)*pframes=(size_t)nframes;
    return 0;
}

/* Сборка синтетического DFF снизу вверх: PROP(81) + DST(FRTE + 3 несжатых
   DSTF: 9408 / 100 / 9408). Возвращает собранный буфер и его длину. */
static uint8_t* build_synth(size_t*plen_out,size_t frame_bytes)
{
    const size_t fb=frame_bytes;
    /* --- PROP (81) --- */
    size_t prop_len=81;
    uint8_t prop[93]; /* 12+81 */
    memcpy(prop,"PROP",4); put_be64(prop+4,prop_len);
    uint8_t*p=prop+12;
    memcpy(p,"SND ",4); p+=4;
    memcpy(p,"FS  ",4); put_be64(p+4,8); put_be64(p+12,2822400ULL); p+=20;
    memcpy(p,"CHNL",4); put_be64(p+4,10); put_be16(p+12,2);
    memcpy(p+14,"LEFT RIG",8); p+=22;
    memcpy(p,"CMPR",4); put_be64(p+4,23);
    memcpy(p+12,"LFDC",4); memset(p+16,0,19); p+=35;
    /* p == prop+12+81 = prop+93 */

    /* --- DSTF-фреймы: [0x00 | данные] --- */
    const size_t lens[3]={fb,100,fb};
    size_t dst_pay=18;
    for(int i=0;i<3;i++) dst_pay+=12+lens[i];

    size_t dst_total=12+dst_pay;
    size_t frm8_size=4+16+(12+prop_len)+dst_total;
    size_t total=12+frm8_size;
    uint8_t*buf=malloc(total);
    if(!buf) return NULL;
    memset(buf,0,total);

    memcpy(buf,"FRM8",4); put_be64(buf+4,frm8_size);
    memcpy(buf+12,"DSD ",4);
    memcpy(buf+16,"FVER",4); put_be64(buf+20,4); put_be32(buf+28,0x01050000u);
    memcpy(buf+32,prop,93);
    size_t o=32+93;
    memcpy(buf+o,"DST ",4); put_be64(buf+o+4,dst_pay); o+=12;
    memcpy(buf+o,"FRTE",4); put_be64(buf+o+4,6);
    put_be32(buf+o+12,3); put_be16(buf+o+16,75); o+=18;
    for(int i=0;i<3;i++){
        memcpy(buf+o,"DSTF",4); put_be64(buf+o+4,lens[i]); o+=12;
        buf[o++]=0x00; /* тип: несжатый */
        memset(buf+o,0,lens[i]-1); o+=lens[i]-1;
    }
    /* o == total */
    if(plen_out)*plen_out=total;
    return buf;
}

/* Эталон для несжатой ветки: на каждый DSTF декодер пишет frame_bytes:
   копия min(len-1, frame_bytes) байтов данных + добивка 0x69 до frame_bytes. */
static uint8_t* build_expected(size_t frame_bytes,size_t*plen_out)
{
    const size_t fb=frame_bytes;
    const size_t lens[3]={fb,100,fb};
    size_t total=3*fb;
    uint8_t*exp=malloc(total);
    if(!exp) return NULL;
    uint8_t zeros[64]; memset(zeros,0,sizeof zeros);
    uint8_t pad[64]; memset(pad,0x69,sizeof pad);
    size_t o=0;
    for(int i=0;i<3;i++){
        size_t n=lens[i]-1; if(n>fb)n=fb;
        while(n>=64){memcpy(exp+o,zeros,64);o+=64;n-=64;}
        if(n){memcpy(exp+o,zeros,n);o+=n;}
        size_t rest=fb-(o%fb);
        while(rest>=64){memcpy(exp+o,pad,64);o+=64;rest-=64;}
        if(rest){memcpy(exp+o,pad,rest);o+=rest;}
    }
    *plen_out=total;
    return exp;
}

static int selftest(void)
{
    size_t frame_bytes=9408; /* DSD64 стерео: 588*64/8*2 */
    size_t slen,elen;
    uint8_t*sb=build_synth(&slen,frame_bytes);
    if(!sb){fprintf(stderr,"selftest: build_synth oom\n");return 1;}
    uint8_t*eb=build_expected(frame_bytes,&elen);
    if(!eb){fprintf(stderr,"selftest: build_expected oom\n");return 1;}

    uint8_t*out=NULL; size_t total=0,nok=0,nfail=0;
    uint32_t fs=0; int ch=0; size_t nfr=0;
    int rc=dff_decode(sb,slen,&out,&total,&nok,&nfail,&fs,&ch,&nfr);
    int fail=0;
    if(rc){fprintf(stderr,"selftest: decode rc=%d\n",rc);fail=1;}
    else{
        printf("selftest: fs=%u ch=%d frames=%zu ok=%zu fail=%zu dsd_bytes=%zu (exp %zu)\n",
               fs,ch,nfr,nok,nfail,total,elen);
        if(total!=elen){printf("selftest: SIZE MISMATCH out=%zu exp=%zu\n",total,elen);fail=1;}
        else{
            size_t mm=0,first=0;
            for(size_t i=0;i<total;i++) if(out[i]!=eb[i]){if(!mm)first=i;mm++;}
            if(mm){printf("selftest: MISMATCH %zu bytes, first @%zu: got %02x exp %02x\n",
                          mm,first,out[first],eb[first]);fail=1;}
        }
    }
    free(sb);free(eb);
    if(out)free(out);
    if(!fail)printf("SELFTEST PASS\n");
    return fail;
}

int main(int argc,char**argv)
{
    if(argc>=2&&!strcmp(argv[1],"--selftest")) return selftest();

    const char*dsd_ref=NULL,*dump=NULL;
    if(argc<2){fprintf(stderr,"usage: %s file.dff [--dsd ref.bin] [--dump out.bin]\n       %s --selftest\n",argv[0],argv[0]);return 2;}
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"--dsd")&&i+1<argc)dsd_ref=argv[++i];
        else if(!strcmp(argv[i],"--dump")&&i+1<argc)dump=argv[++i];
        else{fprintf(stderr,"unknown arg %s\n",argv[i]);return 2;}
    }
    FILE*f=fopen(argv[1],"rb"); if(!f){perror(argv[1]);return 1;}
    fseek(f,0,SEEK_END); long fsz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t*buf=malloc(fsz);
    if(!buf||fread(buf,1,fsz,f)!=(size_t)fsz){fprintf(stderr,"read fail\n");return 1;}
    fclose(f);

    uint8_t*out=NULL; size_t total=0,nok=0,nfail=0;
    uint32_t fs=0; int ch=0; size_t nfr=0;
    int rc=dff_decode(buf,(size_t)fsz,&out,&total,&nok,&nfail,&fs,&ch,&nfr);
    if(rc){fprintf(stderr,"parse/decode error rc=%d\n",rc);return 1;}
    printf("DFF: fs=%u channels=%d\n",fs,ch);
    printf("DST: frames=%zu rate=75\n",nfr);
    printf("decoded: ok=%zu fail=%zu dsd_bytes=%zu\n",nok,nfail,total);

    int rc_all=0;
    if(dsd_ref){
        FILE*r=fopen(dsd_ref,"rb");
        if(!r){perror(dsd_ref);return 1;}
        fseek(r,0,SEEK_END); long rsz=ftell(r); fseek(r,0,SEEK_SET);
        uint8_t*ref=malloc(rsz);
        if(!ref||fread(ref,1,rsz,r)!=(size_t)rsz){fprintf(stderr,"read fail\n");return 1;}
        fclose(r);
        size_t mm=0; size_t cmp=(size_t)rsz<total?(size_t)rsz:total;
        for(size_t i=0;i<cmp;i++) if(out[i]!=ref[i]) mm++;
        if((long)total!=(long)rsz){printf("SIZE MISMATCH: out=%zu ref=%ld\n",total,rsz);rc_all=1;}
        else if(mm){printf("DSD MISMATCH: %zu/%zu bytes differ\n",mm,total);rc_all=1;}
        else printf("DSD MATCH: %zu/%zu bytes\n",total,total);
    }
    if(dump){FILE*o=fopen(dump,"wb");if(!o){perror(dump);return 1;}fwrite(out,1,total,o);fclose(o);}
    free(buf);
    if(out)free(out);
    return rc_all;
}
