// Project-local formatting. Included by stdlib.c, with no external library.
// Supported conversions and limits are documented in stdlib.h.
typedef struct _stdlib_format_output {
    char* dst;
    int64 cap;
    int64 at;
} _stdlib_format_output;

static void _stdlib_format_repeat(_stdlib_format_output* out, char c, int64 n)
{
    if (n <= 0) return;
    int64 available = out->dst && out->cap > out->at + 1 ? out->cap - out->at - 1 : 0;
    int64 written = n < available ? n : available;
    if (written) stdlib__memset(out->dst + out->at, c, written);
    out->at += n;
}
static void _stdlib_format_text(_stdlib_format_output* out, const char* s, int64 n)
{
    if (n <= 0) return;
    int64 available = out->dst && out->cap > out->at + 1 ? out->cap - out->at - 1 : 0;
    int64 written = n < available ? n : available;
    if (written) stdlib__memcpy(out->dst + out->at, s, written);
    out->at += n;
}
static void _stdlib_format_number(_stdlib_format_output* out, uint64 value, int base,
    int upper, char sign, int alternate, int64 width, int64 precision, int left, int zero)
{
    char digits[64], prefix[3];
    int n=0, prefix_n=0;
    const char* alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64 original=value;
    if (value || precision != 0) do {
        uint64 digit;
        value = stdlib__udivmod64(value, (uint)base, &digit);
        digits[n++] = alphabet[digit];
    } while (value);
    if (sign) prefix[prefix_n++]=sign;
    if (alternate && base==16 && original) { prefix[prefix_n++]='0'; prefix[prefix_n++]=upper?'X':'x'; }
    if (alternate && base==8 && (n==0 || digits[n-1]!='0') && precision<=n) precision=n+1;
    int64 leading = precision > n ? precision-n : 0;
    int64 padding = width - prefix_n - leading - n;
    if (padding<0) padding=0;
    if (!left && (!zero || precision>=0)) _stdlib_format_repeat(out,' ',padding);
    _stdlib_format_text(out,prefix,prefix_n);
    if (!left && zero && precision<0) _stdlib_format_repeat(out,'0',padding);
    _stdlib_format_repeat(out,'0',leading);
    while (n) _stdlib_format_text(out,&digits[--n],1);
    if (left) _stdlib_format_repeat(out,' ',padding);
}

// Convert the exact binary64 rational to decimal using base-1e9 limbs. A
// binary64 needs at most 767 significant decimal digits, including subnormals.
// Multiplication by 2 or 5 never overflows uint64. Typical UI numbers need only
// a few limbs; no heap and no float-to-integer conversion of an unchecked value.
static int _stdlib_format_decimal(double value, char* digits, int* point)
{
    union { double d; uint64 u; } bits = {value};
    uint64 mantissa=bits.u & 0xfffffffffffffULL;
    int exponent=(int)((bits.u>>52)&2047);
    if (exponent) { mantissa |= 1ULL<<52; exponent-=1075; }
    else exponent=-1074;
    if (!mantissa) { digits[0]='0'; *point=1; return 1; }
    uint limbs[96]; int count=0;
    do {
        uint64 digit;
        mantissa = stdlib__udivmod64(mantissa, 1000000000u, &digit);
        limbs[count++] = (uint)digit;
    } while (mantissa);
    int scale=exponent<0?-exponent:0;
    uint factor=exponent<0?5:2;
    int steps=exponent<0?-exponent:exponent;
    for (int i=0;i<steps;i++) {
        uint64 carry=0;
        for (int j=0;j<count;j++) {
            uint64 next=(uint64)limbs[j]*factor+carry;
            uint64 digit;
            carry = stdlib__udivmod64(next, 1000000000u, &digit);
            limbs[j] = (uint)digit;
        }
        if (carry) limbs[count++]=(uint)carry;
    }
    int n=0;
    for (int j=count-1;j>=0;j--) {
        uint divisor=100000000;
        if (j==count-1) while (divisor>1 && limbs[j]<divisor) divisor/=10;
        while (divisor) { digits[n++]=(char)('0'+limbs[j]/divisor%10); divisor/=10; }
    }
    *point=n-scale;
    return n;
}
static void _stdlib_format_float(_stdlib_format_output* out, double value, int upper,
    char positive_sign, int alternate, int64 width, int64 precision, int left, int zero)
{
    union { double d; uint64 u; } bits={value};
    char sign=(bits.u>>63)?'-':positive_sign;
    bits.u &= 0x7fffffffffffffffULL;
    if ((bits.u>>52)==2047) {
        const char* text=(bits.u&0xfffffffffffffULL)?(upper?"NAN":"nan"):(upper?"INF":"inf");
        int64 pad=width-3-(sign!=0);
        if (!left) _stdlib_format_repeat(out,' ',pad);
        if (sign) _stdlib_format_text(out,&sign,1);
        _stdlib_format_text(out,text,3);
        if (left) _stdlib_format_repeat(out,' ',pad);
        return;
    }
    if (precision<0) precision=6;
    char digits[800]; int point;
    int n=_stdlib_format_decimal(bits.d,digits,&point);
    int64 keep=(int64)point+precision;
    if (keep<n) {
        int up=0;
        if (keep>=0) {
            int next=digits[keep]-'0', sticky=0;
            for (int i=(int)keep+1;i<n;i++) sticky |= digits[i]!='0';
            up=next>5 || (next==5 && (sticky || (keep && ((digits[keep-1]-'0')&1))));
        }
        if (keep<=0) {
            digits[0]=up?'1':'0'; n=1;
            point=up ? 1-(int)precision : 1;
        } else {
            n=(int)keep;
            int i=n-1;
            while (up && i>=0 && digits[i]=='9') digits[i--]='0';
            if (up && i>=0) digits[i]++;
            else if (up) { digits[0]='1'; for (i=1;i<=n;i++) digits[i]='0'; n++; point++; }
        }
    }
    int64 integers=point>0?point:1;
    int dot=precision>0 || alternate;
    int64 pad=width-integers-precision-dot-(sign!=0);
    if (!left && !zero) _stdlib_format_repeat(out,' ',pad);
    if (sign) _stdlib_format_text(out,&sign,1);
    if (!left && zero) _stdlib_format_repeat(out,'0',pad);
    if (point<=0) _stdlib_format_repeat(out,'0',1);
    else { int take=point<n?point:n; _stdlib_format_text(out,digits,take); _stdlib_format_repeat(out,'0',point-take); }
    if (dot) _stdlib_format_text(out,".",1);
    int64 remaining=precision;
    if (point<0) { int64 zeros=-(int64)point<remaining?-(int64)point:remaining; _stdlib_format_repeat(out,'0',zeros); remaining-=zeros; }
    int start=point>0?point:0;
    int64 take=start<n ? n-start : 0;
    if (take>remaining) take=remaining;
    if (take) _stdlib_format_text(out,digits+start,take);
    _stdlib_format_repeat(out,'0',remaining-take);
    if (left) _stdlib_format_repeat(out,' ',pad);
}

int64 stdlib__vformat(char* dst, int64 cap, const char* fmt, va_list args)
{
    _stdlib_format_output out={dst,cap>0?cap:0,0};
    if (!fmt) fmt="";
    while (*fmt) {
        if (*fmt!='%') { _stdlib_format_text(&out,fmt++,1); continue; }
        fmt++;
        int left=0,zero=0,alternate=0; char sign=0;
        for (;;) {
            if (*fmt=='-') left=1;
            else if (*fmt=='0') zero=1;
            else if (*fmt=='+') sign='+';
            else if (*fmt==' ') { if (!sign) sign=' '; }
            else if (*fmt=='#') alternate=1;
            else break;
            fmt++;
        }
        int64 width=0,precision=-1;
        if (*fmt=='*') { width=va_arg(args,int); fmt++; if (width<0) { left=1; width=-width; } }
        else while (*fmt>='0' && *fmt<='9') { if (width<INT_MAX) width=width*10+(*fmt-'0'); fmt++; }
        if (width>INT_MAX) width=INT_MAX;
        if (*fmt=='.') {
            fmt++; precision=0;
            if (*fmt=='*') { precision=va_arg(args,int); fmt++; if (precision<0) precision=-1; }
            else while (*fmt>='0' && *fmt<='9') { if (precision<INT_MAX) precision=precision*10+(*fmt-'0'); fmt++; }
            if (precision>INT_MAX) precision=INT_MAX;
        }
        char length=0;
        if (*fmt=='l' || *fmt=='h' || *fmt=='j' || *fmt=='z' || *fmt=='t') {
            length=*fmt++;
            if (length=='l' && *fmt=='l') { length='q'; fmt++; }
            if (length=='h' && *fmt=='h') { length='b'; fmt++; }
        }
        char conversion=*fmt;
        if (!conversion) break;
        fmt++;
        if (conversion=='s' || conversion=='c' || conversion=='%') {
            char c; const char* text; int64 n=0;
            if (conversion=='s') {
                text=va_arg(args,const char*); if (!text) text="(null)";
                while ((precision<0 || n<precision) && text[n]) n++;
            } else { c=conversion=='%'?'%':(char)va_arg(args,int); text=&c; n=1; }
            if (!left) _stdlib_format_repeat(&out,' ',width-n);
            _stdlib_format_text(&out,text,n);
            if (left) _stdlib_format_repeat(&out,' ',width-n);
        } else if (conversion=='f' || conversion=='F') {
            _stdlib_format_float(&out,va_arg(args,double),conversion=='F',sign,alternate,width,precision,left,zero);
        } else if (conversion=='d' || conversion=='i' || conversion=='u' || conversion=='x' || conversion=='X' || conversion=='o' || conversion=='p') {
            uint64 value;
            int is_signed=conversion=='d' || conversion=='i';
            if (conversion=='p') { value=(nuint)va_arg(args,void*); alternate=1; }
            else if (is_signed) {
                int64 v;
                if (length=='q' || length=='j') v=va_arg(args,int64);
                else if (length=='l') v=va_arg(args,long);
                else if (length=='z' || length=='t') v=va_arg(args,nint);
                else { int i=va_arg(args,int); v=length=='h'?(short)i:length=='b'?(signed char)i:i; }
                value=v<0?0ULL-(uint64)v:(uint64)v;
                if (v<0) sign='-';
            } else {
                sign=0;
                if (length=='q' || length=='j') value=va_arg(args,uint64);
                else if (length=='l') value=va_arg(args,unsigned long);
                else if (length=='z' || length=='t') value=va_arg(args,nuint);
                else { uint v=va_arg(args,uint); value=length=='h'?(ushort)v:length=='b'?(ubyte)v:v; }
            }
            int base=conversion=='o'?8:(conversion=='x'||conversion=='X'||conversion=='p')?16:10;
            _stdlib_format_number(&out,value,base,conversion=='X',sign,alternate,width,precision,left,zero);
        } else { _stdlib_format_text(&out,"%",1); _stdlib_format_text(&out,&conversion,1); }
    }
    if (dst && cap>0) dst[out.at<cap?out.at:cap-1]=0;
    return out.at;
}
