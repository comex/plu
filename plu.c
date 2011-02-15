#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <errno.h>

static void show(FILE *fp, CFTypeRef object) {
    CFShow(object);
}

static bool dots(void *object, char *expr, void **out) {
    bool set = *out != NULL;
    char type = '.';
    char *eos = expr + strlen(expr);
    while(1) {
        char *next;
        char next_type;
        if(type != 0 && type != '-') {
            if(*expr == '"') {
                expr++;
                next = strchr(expr, '"');
                if(!next) {
                    fprintf(stderr, "Mismatched quotes\n");
                    return false;
                }
                *next = 0;
                next++;
                if(type == '[') next++;
            } else {
                next = strpbrk(expr + 1, "[.") ?: eos;
            }
            next_type = *next;
        }
        switch(type) {
        case '[':
            if(next[-1] != ']') {
                fprintf(stderr, "Syntax error\n");
                return false;
            }
            next[-1] = 0;
            break;
        case '.':
            *next = 0;
            break;
        case 0:
        case '-':
            if(set) {
                fprintf(stderr, "Nothing will come of nothing.\n");
                return false;
            } else {
                *out = object;
                return true;
            }
        default:
            fprintf(stderr, "Syntax error: %c %s\n", type, expr);
            return false;
        }

        bool set_now = set && next_type == 0;
        CFTypeID cftype = CFGetTypeID(object);
        if(cftype == CFArrayGetTypeID()) {
            long long l;
            if(set && *expr == 0) {
                l = CFArrayGetCount(object);
            } else {
                errno = 0;
                char *end;
                l = strtoll(expr, &end, 0);
                if(errno || *end) {
                    show(stderr, object); 
                    fprintf(stderr, "Not a number: <%s>\n", expr);
                    return false;
                }
            }

            long long count = (long long) CFArrayGetCount(object);
            if(l < 0 || (set ? (l > count) : (l >= count))) {
                show(stderr, object);
                fprintf(stderr, "Out of range: %lld\n", l);
                return false;
            }

            if(set_now) {
                if(l == count) {
                    CFArrayAppendValue(object, *out);
                } else {
                    CFArraySetValueAtIndex(object, (CFIndex) l, *out);
                }
                return true;
            } else {
                object = (void *) CFArrayGetValueAtIndex(object, (CFIndex) l);
            }
        } else if(cftype == CFDictionaryGetTypeID()) {
            CFStringRef str = CFStringCreateWithCString(NULL, expr, kCFStringEncodingUTF8);
            if(!str) {
                fprintf(stderr, "Invalid string: %s\n", expr);
                return false;
            }

            if(set_now) {
                CFDictionarySetValue(object, str, *out);
                CFRelease(str);
                return true;
            } else {
                void *new_object;
                Boolean success = CFDictionaryGetValueIfPresent(object, str, (void *) &new_object);
                CFRelease(str);
                if(success) {
                    object = new_object;
                } else {
                    show(stderr, object);
                    fprintf(stderr, "No such key: %s\n", expr);
                    return false;
                }
            }
        } else {
            show(stderr, object);
            fprintf(stderr, "Can't index (%s) into unknown type\n", expr);
            return false;
        }
            

        expr = next + 1;
        type = next_type;
    }
}

static void usage() {
    fprintf(stderr, "Usage: plu filename|value options...\n"
                    "Options:\n"
                    "  -s key value    set value\n"
                    "  key             get value\n"
                    "  -w out.plist    write\n"
                    "  -x out.plist    write XML\n"
                    "\n"
                    " Key example: prop[5].foo\n"
                    " Values written as old-style property lists.\n");
    exit(1);
}

static char *urlerror_to_str(CFURLError error) {
    switch(error) {
    case kCFURLUnknownError: return "unknown error";
    case kCFURLUnknownSchemeError: return "unknown scheme";
    case kCFURLResourceNotFoundError: return "No such file or directory";
    case kCFURLResourceAccessViolationError: return "Permission denied";
    case kCFURLRemoteHostUnavailableError: return "remote host unavailable";
    case kCFURLImproperArgumentsError: return "improper arguments";
    case kCFURLUnknownPropertyKeyError: return "unknown property key";
    case kCFURLPropertyKeyUnavailableError: return "unavailable property key";  
    case kCFURLTimeoutError: return "timeout";
    }
    char *buf;
    asprintf(&buf, "unknown error %d", error);
    return buf;

}

static char *cferror_to_string(CFErrorRef error) {
    CFStringRef estring = CFErrorCopyDescription(error);
    char *buf = malloc(1024);
    if(!CFStringGetCString(estring, buf, 1024, kCFStringEncodingUTF8)) {
        strcpy(buf, "(conversion fail)");
    }
    return buf;
}

static CFURLRef filename_to_url(const char *filename) {
    CFStringRef string = CFStringCreateWithCString(NULL, filename, kCFStringEncodingUTF8);
    if(!string) {
        fprintf(stderr, "Couldn't create string\n");
        exit(1);
    }
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, string, kCFURLPOSIXPathStyle, false);
    CFRelease(string);
    if(!url) {
        fprintf(stderr, "Invalid path: %s\n", filename);
        exit(1);
    }
    return url;
}


int main(int argc, char **argv) {
    char *filename = argv[1];
    if(!filename) usage();

    CFPropertyListRef plist;
    CFPropertyListFormat format;
    CFErrorRef cferror;

    switch(filename[0]) {
    case '(':
    case '{':
    case '"':
    case '<': {
        // inline property list
        CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, filename, strlen(filename), kCFAllocatorNull);
        if(!data) {
            fprintf(stderr, "Couldn't create data\n");
            exit(1);
        }
        
        plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListMutableContainersAndLeaves, NULL, &cferror);
        format = kCFPropertyListXMLFormat_v1_0;
        break;
    }
    default: {
        CFURLRef url = filename_to_url(filename);
        CFDataRef data;
        SInt32 error;
        if(!CFURLCreateDataAndPropertiesFromResource(NULL, url, &data, NULL, NULL, &error)) {
            fprintf(stderr, "Couldn't open %s: %s\n", filename, urlerror_to_str(error));
            exit(1);
        }
        
        plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListMutableContainersAndLeaves, &format, &cferror);
    }
    }
        
    if(!plist) {
        fprintf(stderr, "Couldn't parse property list: %s\n", cferror_to_string(cferror));
        exit(1);
    }

    char **argptr = &argv[2];
    while(*argptr) {
        if(!strcmp(*argptr, "-s")) {
            if(!argptr[1] || !argptr[2]) usage();
            char *key = argptr[1];
            char *value = argptr[2];

            CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, value, strlen(value), kCFAllocatorNull);
            if(!data) {
                fprintf(stderr, "Couldn't create data\n");
                exit(1);
            }
            CFPropertyListRef valuepl = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, &cferror);
            if(!valuepl) {
                fprintf(stderr, "Invalid value %s: %s\n", value, cferror_to_string(cferror));
                exit(1);
            }

            if(!dots((void *) plist, key, (void *) &valuepl)) exit(1);

            CFRelease(data);
            CFRelease(valuepl);

            argptr += 3;
        } else if(!strcmp(*argptr, "-w") || !strcmp(*argptr, "-x")) {
            if((*argptr)[1] == 'x') {
                format = kCFPropertyListXMLFormat_v1_0;
            }

            if(!argptr[1]) usage();
            CFURLRef url2 = strcmp(argptr[1], "-") ? filename_to_url(argptr[1]) : NULL;

            if(format == kCFPropertyListOpenStepFormat) {
                fprintf(stderr, "Note: converting OpenStep format to XML\n");
                format = kCFPropertyListXMLFormat_v1_0;
            }
            CFDataRef data = CFPropertyListCreateData(NULL, plist, format, 0, &cferror);
            if(!data) {
                fprintf(stderr, "Couldn't create data: %s\n", cferror_to_string(cferror));
                exit(1);
            }
           
            if(url2) {
                SInt32 error;
                if(!CFURLWriteDataAndPropertiesToResource(url2, data, NULL, &error)) {
                    fprintf(stderr, "Couldn't write %s: %s\n", argptr[1], urlerror_to_str(error));
                    exit(1);
                }
                CFRelease(url2);
            } else {
                write(1, CFDataGetBytePtr(data), CFDataGetLength(data));
            }
            CFRelease(data);

            argptr += 2;
        } else {
            void *out = NULL;
            if(!dots((void *) plist, *argptr, &out)) exit(1);
            show(stdout, out);

            argptr++;
        }
    }
    return 0;
}
