#include "pdf.h"
#include "hotspot.h"
#include "leak.h"
#include "optimize.h"
#include "runtime.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/* =====================================================================
 * 原生 PDF（含中文）生成器
 *
 * 策略（可靠优先，避免复杂字形子集重建）：
 *  1) 读取系统 simhei.ttf 完整内嵌为 FontFile2（约 9.7MB，无需重建子表）；
 *  2) 解析其 cmap，把报告的 UTF-8 文本转成"原始 GID"；
 *  3) Type0 字体 + Identity-H + CIDToGIDMap=/Identity，文本流内写 2 字节 GID；
 *  4) 字体缺失/解析失败 -> 降级为 Helvetica（标准字体免内嵌）纯 ASCII，
 *     非 ASCII 字符显示为 '?'，数据（进程名英文部分/数值）尽量保留。
 * ===================================================================== */

/* ---------- 大端读写 ---------- */
static uint16_t r16(const uint8_t*p){return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t r32(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}

/* ---------- 动态字节缓冲 ---------- */
typedef struct buf{uint8_t*d;size_t n,cap;}buf;
static void bi(buf*b,size_t c){b->n=0;b->cap=c?c:64;b->d=(uint8_t*)malloc(b->cap);}
static void bf(buf*b){free(b->d);b->d=NULL;b->n=b->cap=0;}
static void bput(buf*b,const void*v,size_t n){
    if(b->n+n>b->cap){size_t nc=b->cap?b->cap:64;while(nc<b->n+n)nc*=2;b->d=(uint8_t*)realloc(b->d,nc);b->cap=nc;}
    memcpy(b->d+b->n,v,n);b->n+=n;
}
static void bputs(buf*b,const char*s){bput(b,s,strlen(s));}
static void bprintf(buf*b,const char*fmt,...){
    char tmp[1200];va_list ap;va_start(ap,fmt);vsnprintf(tmp,sizeof tmp,fmt,ap);va_end(ap);
    bputs(b,tmp);
}

/* ---------- Unicode 集合 ---------- */
typedef struct uset{uint32_t*v;int n,cap;}uset;
static void usi(uset*u){u->cap=256;u->n=0;u->v=(uint32_t*)malloc((size_t)u->cap*sizeof(uint32_t));}
static void usf(uset*u){free(u->v);u->v=NULL;}
static void usadd(uset*u,uint32_t c){
    for(int i=0;i<u->n;i++)if(u->v[i]==c)return;
    if(u->n>=u->cap){u->cap*=2;u->v=(uint32_t*)realloc(u->v,(size_t)u->cap*sizeof(uint32_t));}
    u->v[u->n++]=c;
}
static void usutf8(uset*u,const char*s){
    if(!s)return;
    const uint8_t*p=(const uint8_t*)s;
    while(*p){
        uint8_t c=*p;uint32_t cp;int ad;
        if(c<0x80){cp=c;ad=1;}
        else if((c&0xE0)==0xC0){cp=c&0x1F;ad=2;}
        else if((c&0xF0)==0xE0){cp=c&0x0F;ad=3;}
        else if((c&0xF8)==0xF0){cp=c&0x07;ad=4;}
        else{cp=0xFFFD;ad=1;}
        for(int i=1;i<ad&&p[i];i++)cp=(cp<<6)|(p[i]&0x3F);
        usadd(u,cp);p+=ad;
    }
}

/* ---------- TrueType 表定位 ---------- */
typedef struct ttloc{uint32_t off,len;}ttloc;
typedef struct tfont{
    const uint8_t*data;size_t size;
    ttloc cmap,head,hhea,maxp,hmtx;
    int cmap_fmt;uint32_t cmap_sub;
    int nloc,num_glyphs,num_hmetrics;
}tfont;
static ttloc tf_find(const tfont*f,const char*tag){
    ttloc r={0,0};
    if(f->size<12)return r;
    uint16_t n=r16(f->data+4);
    for(int i=0;i<n;i++){
        const uint8_t*rec=f->data+12+i*16;
        if(!memcmp(rec,tag,4)){r.off=r32(rec+8);r.len=r32(rec+12);break;}
    }
    return r;
}
static void tf_load(tfont*f,const uint8_t*d,size_t n){
    memset(f,0,sizeof(*f));f->data=d;f->size=n;
    f->cmap=tf_find(f,"cmap");f->head=tf_find(f,"head");
    f->hhea=tf_find(f,"hhea");f->maxp=tf_find(f,"maxp");f->hmtx=tf_find(f,"hmtx");
    f->nloc=r16(d+f->head.off+50);
    f->num_glyphs=r16(d+f->maxp.off+4);
    f->num_hmetrics=r16(d+f->hhea.off+34);
}
static void cmap_pick(tfont*f){
    if(f->cmap.len<4)return;
    uint16_t n=r16(f->data+f->cmap.off+2);
    int best=-1;uint32_t bo=0;int bfmt=0;
    for(int i=0;i<n;i++){
        if(f->cmap.len<4+(size_t)(i+1)*8)break;
        const uint8_t*rec=f->data+f->cmap.off+4+i*8;
        uint16_t plat=r16(rec),enc=r16(rec+2);
        uint32_t off=r32(rec+4);
        if((size_t)off+2>f->cmap.len)continue;
        int ff=r16(f->data+f->cmap.off+off);
        int sc=0;
        if(ff==12&&plat==3&&(enc==1||enc==10))sc=300;
        else if(ff==4&&plat==3)sc=200;
        else if(ff==12)sc=150;
        if(sc>best){best=sc;bo=off;bfmt=ff;}
    }
    f->cmap_fmt=bfmt;f->cmap_sub=bo;
}
/* 码点 -> 原始 GID */
static uint32_t cp_gid(const tfont*f,uint32_t cp){
    if(!f->cmap_fmt)return 0;
    if(f->cmap_fmt==12){
        const uint8_t*p=f->data+f->cmap.off+f->cmap_sub;
        uint32_t g=r32(p+12);
        for(uint32_t i=0;i<g;i++){
            const uint8_t*gr=p+16+i*12;
            uint32_t st=r32(gr),en=r32(gr+4),gid=r32(gr+8);
            if(cp>=st&&cp<=en)return gid+(cp-st);
        }
        return 0;
    }
    /* format 4 */
    const uint8_t*cmap=f->data+f->cmap.off;
    const uint8_t*p=cmap+f->cmap_sub;
    uint16_t segX2=r16(p+6);uint16_t seg=(uint16_t)(segX2/2);
    const uint8_t*endCode=p+14;
    const uint8_t*startCode=endCode+segX2+2;
    const uint8_t*idDelta=startCode+segX2;
    const uint8_t*idRange=idDelta+segX2;
    for(uint16_t i=0;i<seg;i++){
        uint16_t sc=r16(startCode+i*2),ec=r16(endCode+i*2);
        if(cp>=sc&&cp<=ec){
            int16_t delta=(int16_t)r16(idDelta+i*2);
            uint16_t ro=r16(idRange+i*2);
            if(ro==0)return (uint32_t)((cp+delta)&0xFFFF);
            uint32_t par=(uint32_t)((idRange+i*2)-cmap);
            uint32_t ga=par+ro+2*(cp-sc);
            if(ga+2<=f->cmap.len){uint16_t gi=r16(cmap+ga);if(gi)return (uint32_t)((gi+delta)&0xFFFF);}
            return 0;
        }
    }
    return 0;
}
/* 字形宽度(advanceWidth, 每1000em) */
static double gid_width(const tfont*f,uint32_t gid){
    uint16_t aw=1000;
    if(f->num_hmetrics&&gid<(uint32_t)f->num_hmetrics)aw=r16(f->data+f->hmtx.off+(size_t)gid*4);
    else if(f->num_hmetrics)aw=r16(f->data+f->hmtx.off+(size_t)(f->num_hmetrics-1)*4);
    uint16_t upem=r16(f->data+f->head.off+18);if(!upem)upem=1000;
    return (double)aw*1000.0/upem;
}

/* =====================================================================
 * 文本渲染辅助
 * ===================================================================== */
/* 收集一行文本的所有 GID（cjk）或原始字节（ascii）到数组，返回个数 */
static int utf8_to_gids(const tfont*f,int cjk,const char*s,uint8_t*out,int cap){
    const uint8_t*p=(const uint8_t*)s;int n=0;
    while(*p&&n<cap-4){
        uint8_t b=*p;uint32_t cp;int ad;
        if(b<0x80){cp=b;ad=1;}else if((b&0xE0)==0xC0){cp=b&0x1F;ad=2;}
        else if((b&0xF0)==0xE0){cp=b&0x0F;ad=3;}else if((b&0xF8)==0xF0){cp=b&0x07;ad=4;}
        else{cp=0xFFFD;ad=1;}
        for(int i=1;i<ad&&p[i];i++)cp=(cp<<6)|(p[i]&0x3F);
        p+=ad;
        if(cjk){
            uint32_t g=cp_gid(f,cp);
            out[n++]= (uint8_t)(g>>8);out[n++]= (uint8_t)(g&0xFF);
        }else{
            out[n++]=(cp<0x80)?(uint8_t)cp:(uint8_t)'?';
        }
    }
    return n;
}
/* PDF hex string：把 GID 二进制字节编码成 ASCII hex 文本（每字节 2 个 hex 字符）。
 * <...> 内必须是 hex 字符，否则阅读器报语法错误。 */
static void put_hex(buf*b,const uint8_t*g,int n){
    static const char he[]="0123456789ABCDEF";
    char c[2];
    for(int i=0;i<n;i++){c[0]=he[g[i]>>4];c[1]=he[g[i]&15];bput(b,c,2);}
}
static double text_width(const tfont*f,int cjk,const char*s,int fontsize){
    uint8_t g[4094];int n=utf8_to_gids(f,cjk,s,g,(int)sizeof g);
    double w=0;
    if(cjk){
        for(int i=0;i<n-1;i+=2){
            uint32_t gid=((uint32_t)g[i]<<8)|g[i+1];
            w+=gid_width(f,gid)*fontsize/1000.0;
        }
    }else w=(double)n*fontsize*0.48;
    return w;
}

/* 把 hex 文本写入 pdf（用 (<hex> ) Tj 的形式不需要，Identity-H 用 <hex>） */
static void pdf_show_buf(buf*txt){
    bputs(txt,"x"); /* 占位，避免未使用告警 */
}

/* =====================================================================
 * PDF 组装（跟踪对象偏移）
 * ===================================================================== */
typedef struct pdfassemb{ buf body; long offs[16]; int nobj; }pdfassem;

static void a_begin_obj(pdfassem*a,int id){
    if(id>a->nobj)a->nobj=id;
    a->offs[id]= (long)a->body.n;
    bprintf(&a->body,"%d 0 obj\n",id);
}
static void a_end_obj(pdfassem*a){bputs(&a->body,"\nendobj\n");}

char*pdf_build_report(sampler*s,const char*font_path,size_t*out_len){
    sys_mem sy;sampler_lock(s);sy=s->sys;
    hotspot_item hh[15];int hn=hotspot_rank(s,"private",15,hh,15);
    leak_item li[10];int ln=leak_detect(s,li,10);
    suggest_item sug[20];int sn=optimize_analyze(s,sug,20);
    sampler_unlock(s);

    /* 收集字符集 */
    uset u;usi(&u);
    const char*labels[]={"内存优化报告","系统总览","内存占用热点分析(按私有字节)","泄漏检测",
        "内存优化建议","生成时间","物理内存总量:","可用物理内存:","提交限额/已用:","缓存:",
        "系统句柄/线程:","排名","进程","运行时","私有(MB)","工作集(MB)","峰值(MB)","占比",
        "类型","速率(KB/min)","历时(秒)","严重度","私有字节","句柄","高","中","低",
        "未发现明显泄漏趋势。","当前无明显优化项。","MB","%","KB/min","秒"};
    for(size_t k=0;k<sizeof labels/sizeof labels[0];k++)usutf8(&u,labels[k]);
    for(int i=0;i<hn;i++){usutf8(&u,hh[i].name);usutf8(&u,runtime_label((rt_kind)hh[i].rt));}
    for(int i=0;i<ln;i++)usutf8(&u,li[i].name);
    for(int i=0;i<sn;i++){usutf8(&u,sug[i].title);usutf8(&u,sug[i].reason);}
    /* 运行时数值/后缀里的可打印 ASCII（数字、点、百分号等），确保其 GID 也有 ToUnicode 映射 */
    for(uint32_t c=0x20;c<=0x7E;c++)usadd(&u,c);

    /* 读字体 */
    const char*fp=font_path&&font_path[0]?font_path:"C:\\Windows\\Fonts\\simhei.ttf";
    FILE*fd=fopen(fp,"rb");
    uint8_t*fdata=NULL;size_t fsize=0;tfont ft;int font_ok=0;
    if(fd){
        fseek(fd,0,SEEK_END);long sz=ftell(fd);fseek(fd,0,SEEK_SET);
        if(sz>0&&sz<(long)(150*1024*1024)){
            fdata=(uint8_t*)malloc((size_t)sz);
            if(fdata&&fread(fdata,1,(size_t)sz,fd)==(size_t)sz){tf_load(&ft,fdata,(size_t)sz);cmap_pick(&ft);font_ok=1;fsize=(size_t)sz;}
        }
        fclose(fd);
    }
    int cjk=font_ok&&(ft.cmap_fmt==4||ft.cmap_fmt==12);
    if(font_ok&&!cjk){free(fdata);fdata=NULL;font_ok=0;}
    if(!font_ok){ /* 全部用 ascii 模式 */ }
    int usefont=cjk?1:0; /* 1=内嵌simhei, 0=Helvetica */

    /* 组装 PDF 对象 */
    pdfassem A; bi(&A.body,1<<20); memset(A.offs,0,sizeof A.offs); A.nobj=0;
    /* PDF 文件必须以 %PDF-1.x 头开始（规范要求），否则标准阅读器拒绝打开。
       二进制注释行使阅读器把文件当二进制而非文本处理。 */
    bputs(&A.body,"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n");

    /* ---- obj1 catalog ---- */
    a_begin_obj(&A,1);bputs(&A.body,"<< /Type /Catalog /Pages 2 0 R >>");a_end_obj(&A);
    /* ---- obj2 pages ---- */
    a_begin_obj(&A,2);bputs(&A.body,"<< /Type /Pages /Kids [3 0 R] /Count 1 >>");a_end_obj(&A);

    /* ---- 内容流（先构建，后作为 obj7）---- */
    buf content;bi(&content,16384);
    /* 字体与字号：统一 /F1 */
    double size=11;
    int lineh=18;
    double y=795;
    double mx=45; /* 左边距 */

    bprintf(&content,"BT /F1 %g Tf 1 0 0 1 %g %g Tm\n",size,mx,y);
    /* 标题(16pt) */
    {
        uint8_t g[4094];int n=utf8_to_gids(&ft,cjk,"内存优化报告",g,sizeof g);
        bprintf(&content,"/F1 16 Tf ");bputs(&content,"<");put_hex(&content,g,(size_t)n);bputs(&content,"> Tj\n");
        bprintf(&content,"/F1 %g Tf\n",size);
    }
    y-=24;

    /* 定义局部文本写入宏函数（用内联函数写） */
    /* 用 helper：写一行到 content 并推进 y */
    /* 由于 C 里闭包困难，用宏 */
    #define PUTLINE(str_) do{ \
        uint8_t g[4094];int n_=utf8_to_gids(cjk?&ft:NULL,cjk,str_,g,sizeof g); \
        bprintf(&content,"1 0 0 1 %g %g Tm ",mx,y);bputs(&content,"<");put_hex(&content,g,(size_t)n_);bputs(&content,"> Tj\n"); \
        y-=lineh; }while(0)

    /* 系统总览 */

    /* 这里由于 hex 需要 utf8_to_gids，直接内联 */
    {
        uint8_t g[4094];int n=utf8_to_gids(&ft,cjk,"系统总览",g,sizeof g);
        bprintf(&content,"1 0 0 1 %g %g Tm <",mx,y);put_hex(&content,g,(size_t)n);bputs(&content,"> Tj\n");y-=24;
    }
    {
        char line[512];
        snprintf(line,sizeof line,"物理内存总量: %.0f MB", (double)sy.phys_total/1048576.0);PUTLINE(line);
        snprintf(line,sizeof line,"可用物理内存: %.0f MB (%.1f%%)",(double)sy.phys_avail/1048576.0, sy.phys_total?(double)sy.phys_avail/sy.phys_total*100:0);PUTLINE(line);
        snprintf(line,sizeof line,"提交限额/已用: %.0f / %.0f MB",(double)sy.commit_used/1048576.0,(double)sy.commit_limit/1048576.0);PUTLINE(line);
        snprintf(line,sizeof line,"缓存: %.0f MB",(double)sy.cache_bytes/1048576.0);PUTLINE(line);
        snprintf(line,sizeof line,"系统句柄/线程: %u / %u",sy.handle_count,sy.thread_count);PUTLINE(line);
    }
    y-=8;
    /* 热点 */
    {
        uint8_t g[4094];int n=utf8_to_gids(&ft,cjk,"内存占用热点分析(按私有字节)",g,sizeof g);
        bprintf(&content,"1 0 0 1 %g %g Tm <",mx,y);put_hex(&content,g,(size_t)n);bputs(&content,"> Tj\n");y-=20;
        char line[512];
        snprintf(line,sizeof line,"%-2s %-18s %-10s %7s %8s %8s %5s","#"  ,"进程","运行时","私有MB","工作集","峰值","占比");
        (void)line;
        /* 表头 */
        {
            uint8_t h2[4094];int n2=utf8_to_gids(&ft,cjk,"# 进程 运行时 私有MB 工作集 峰值MB 占比",h2,sizeof h2);
            bprintf(&content,"1 0 0 1 %g %g Tm <",mx,y);put_hex(&content,h2,(size_t)n2);bputs(&content,"> Tj\n");y-=18;
        }
        for(int i=0;i<hn;i++){
            char row[300];
            snprintf(row,sizeof row,"%d %s %s %.0f %.0f %.0f %.0f%%",i+1,hh[i].name,
                     runtime_label((rt_kind)hh[i].rt),hh[i].private_mb,hh[i].ws_mb,hh[i].peak_mb,hh[i].share*100);
            PUTLINE(row);
        }
    }
    y-=8;
    /* 泄漏 */
    {
        uint8_t g[4094];int n=utf8_to_gids(&ft,cjk,"泄漏检测",g,sizeof g);
        bprintf(&content,"1 0 0 1 %g %g Tm <",mx,y);put_hex(&content,g,(size_t)n);bputs(&content,"> Tj\n");y-=20;
        if(ln==0){uint8_t g2[4094];int n2=utf8_to_gids(&ft,cjk,"未发现明显泄漏趋势。",g2,sizeof g2);
            bprintf(&content,"1 0 0 1 %g %g Tm <",mx,y);put_hex(&content,g2,(size_t)n2);bputs(&content,"> Tj\n");y-=18;}
        char line[512];
        for(int i=0;i<ln;i++){
            snprintf(line,sizeof line,"%s %s %.1f KB/min R2=%.2f %s",li[i].name,
                     li[i].kind==0?"私有字节":"句柄",li[i].slope_kb_per_min,li[i].r2,
                     (li[i].severity>=4?"高":(li[i].severity>=2?"中":"低")));
            PUTLINE(line);
        }
    }
    y-=8;
    /* 建议 */
    {
        uint8_t g[4094];int n=utf8_to_gids(&ft,cjk,"内存优化建议",g,sizeof g);
        bprintf(&content,"1 0 0 1 %g %g Tm <",mx,y);put_hex(&content,g,(size_t)n);bputs(&content,"> Tj\n");y-=20;
        if(sn==0){uint8_t g2[4094];int n2=utf8_to_gids(&ft,cjk,"当前无明显优化项。",g2,sizeof g2);
            bprintf(&content,"1 0 0 1 %g %g Tm <",mx,y);put_hex(&content,g2,(size_t)n2);bputs(&content,"> Tj\n");y-=18;}
        char line[600];
        for(int i=0;i<sn;i++){
            snprintf(line,sizeof line,"[%s] %s: %s",(sug[i].severity>=4?"高":(sug[i].severity>=2?"中":"低")),
                     sug[i].title,sug[i].reason);
            PUTLINE(line);
        }
    }
    bputs(&content,"ET\n");

    #undef PUTLINE

    /* obj7: content */
    a_begin_obj(&A,7);
    bprintf(&A.body,"<< /Length %llu >>\nstream\n",(unsigned long long)content.n);
    bput(&A.body,content.d,content.n);
    bputs(&A.body,"\nendstream");a_end_obj(&A);

    /* ---- obj3 page ---- */
    a_begin_obj(&A,3);
    bputs(&A.body,"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 ");
    bprintf(&A.body,"%d 0 R", usefont?4:8);
    bputs(&A.body," >> >> /Contents 7 0 R >>");
    a_end_obj(&A);

    if(usefont){
        /* ---- obj4: Type0 font ---- */
        a_begin_obj(&A,4);
        bputs(&A.body,"<< /Type /Font /Subtype /Type0 /BaseFont /SimHei /Encoding /Identity-H");
        bputs(&A.body," /DescendantFonts [5 0 R] /ToUnicode 9 0 R >>");a_end_obj(&A);
        /* ---- obj5: CIDFontType2 ---- */
        a_begin_obj(&A,5);
        bputs(&A.body,"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /SimHei");
        bputs(&A.body," /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>");
        bputs(&A.body," /FontDescriptor 6 0 R /CIDToGIDMap /Identity >>");a_end_obj(&A);
        /* ---- obj6: FontDescriptor ---- */
        a_begin_obj(&A,6);
        bprintf(&A.body,"<< /Type /FontDescriptor /FontName /SimHei /Flags 4 /FontBBox [0 -200 1000 900]");
        bprintf(&A.body,"/ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80");
        bprintf(&A.body," /FontFile2 %d 0 R >>",8);a_end_obj(&A);
    }else{
        /* 降级：用 Helvetica（免内嵌）——页面 Font 引用 obj8 */
        a_begin_obj(&A,8);
        bputs(&A.body,"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");a_end_obj(&A);
    }

    /* ---- obj8 默认：若内嵌字体则作为 FontFile2，否则 Helvetica 已用 obj8 ---- */
    if(usefont){
        a_begin_obj(&A,8);
        bprintf(&A.body,"<< /Length1 %llu /Length %llu >>\nstream\n",
                (unsigned long long)fsize,(unsigned long long)fsize);
        if(fdata)bput(&A.body,fdata,fsize);
        bputs(&A.body,"\nendstream");a_end_obj(&A);
    }

    /* ToUnicode CMap（obj9）仅内嵌字体时生成。
     * 采用 Identity-H + CIDToGIDMap=/Identity，内容流内的 2 字节码即 GID，
     * 因此 CMap 把每个使用的字形的 GID 映射到其 Unicode。长度必须与实际流字节数
     * 一致，否则阅读器解析该对象时找不到 endstream 而报错。 */
    if (usefont) {
        buf toc; bi(&toc, 4096);
        /* 先统计有多少码点在字体中有真实 GID，作为 beginbfchar 行数 */
        int ng = 0;
        for (int i = 0; i < u.n; i++) if (cp_gid(&ft, u.v[i])) ng++;
        bputs(&toc, "/CIDInit /ProcSet findresource begin\n");
        bputs(&toc, "12 dict begin begincmap\n");
        bputs(&toc, "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n");
        bputs(&toc, "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n");
        bputs(&toc, "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
        bprintf(&toc, "%d beginbfchar\n", ng);
        for (int i = 0; i < u.n; i++) {
            uint32_t cp = u.v[i];
            uint32_t gid = cp_gid(&ft, cp);
            if (!gid) continue;
            bprintf(&toc, "<%04X> <", gid);
            if (cp <= 0xFFFF) {
                bprintf(&toc, "%04X", cp);
            } else {
                /* 非平面0字符编码为 UTF-16 代理对 */
                uint32_t v = cp - 0x10000;
                uint32_t hi = 0xD800 | ((v >> 10) & 0x3FF);
                uint32_t lo = 0xDC00 | (v & 0x3FF);
                bprintf(&toc, "%04X%04X", hi, lo);
            }
            bputs(&toc, ">\n");
        }
        bputs(&toc, "endbfchar\nendcmap\nCMapName currentdict /CMap defineresource pop\nend\nend");
        a_begin_obj(&A, 9);
        bprintf(&A.body, "<< /Length %llu >>\nstream\n", (unsigned long long)toc.n);
        bput(&A.body, toc.d, toc.n);
        bputs(&A.body, "\nendstream");
        a_end_obj(&A);
        bf(&toc);
    }

    /* ---- xref ---- */
    long xref_off=(long)A.body.n;
    bprintf(&A.body,"xref\n0 %d\n",A.nobj+1);
    bputs(&A.body,"0000000000 65535 f \n");
    for(int i=1;i<=A.nobj;i++)
        bprintf(&A.body,"%010ld 00000 n \n",A.offs[i]);
    bputs(&A.body,"trailer\n<< /Size ");
    bprintf(&A.body,"%d",A.nobj+1);
    bputs(&A.body," /Root 1 0 R >>\nstartxref\n");
    bprintf(&A.body,"%ld\n%%%%EOF\n",xref_off);

    /* 完整字节数组 */
    size_t L=A.body.n;
    if(out_len)*out_len=L;
    char*res=(char*)malloc(L+1);
    if(res){memcpy(res,A.body.d,L);res[L]='\0';}
    else{bf(&A.body);bf(&content);usf(&u);free(fdata);return NULL;}

    bf(&A.body);bf(&content);usf(&u);free(fdata);
    return res;
}