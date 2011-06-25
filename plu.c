#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>

static void show(FILE *fp, CFTypeRef object) {
    CFShow(object);
}

enum {
    MODE_GET,
    MODE_SET,
    MODE_REMOVE
};

static bool dots(void *object, char *expr, int mode, void *out) {
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
            if(mode == MODE_GET) {
                *((void **) out) = object;
                return true;
            } else {
                fprintf(stderr, "Nothing will come of nothing; try again.\n");
                return false;
            }
        default:
            fprintf(stderr, "Syntax error: %c %s\n", type, expr);
            return false;
        }

        bool set_now = next_type == 0;
        CFTypeID cftype = CFGetTypeID(object);
        if(cftype == CFArrayGetTypeID()) {
            long long l;
            if(mode == MODE_SET && *expr == 0) {
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
            if(l < 0 || (mode == MODE_SET ? (l > count) : (l >= count))) {
                show(stderr, object);
                fprintf(stderr, "Out of range: %lld\n", l);
                return false;
            }

            if(set_now && mode == MODE_SET) {
                if(l == count) {
                    CFArrayAppendValue(object, out);
                } else {
                    CFArraySetValueAtIndex(object, (CFIndex) l, out);
                }
                return true;
            } else if(set_now && mode == MODE_REMOVE) {
                CFArrayRemoveValueAtIndex(object, (CFIndex) l); 
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

            if(set_now && mode == MODE_SET) {
                CFDictionarySetValue(object, str, out);
                CFRelease(str);
                return true;
            } else if(set_now && mode == MODE_REMOVE) {
                CFDictionaryRemoveValue(object, str);
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
                    "  -r key          remove value\n"
                    "  -w out.plist    write\n"
                    "  -x out.plist    write XML\n"
                    "\n"
                    " Key example: prop[5].foo\n"
                    " Values should be written as old-style property lists.\n");
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

static void serialize_openstep_recurse(CFPropertyListRef plist, CFStringRef indent, CFMutableStringRef output) {
    CFTypeID type = CFGetTypeID(plist);
    if(type == CFDataGetTypeID()) {
        CFStringAppend(output, CFSTR("<"));
        CFDataRef data = plist;
        const UInt8 *bytes = CFDataGetBytePtr(data);
        CFIndex length = CFDataGetLength(data);
        for(CFIndex i = 0; i < length; i++) {
            CFStringAppendFormat(output, NULL, CFSTR("%02x"), bytes[i]);
        }
        CFStringAppend(output, CFSTR(">"));
        return;
    }
    if(type == CFStringGetTypeID()) {
        CFStringAppend(output, CFSTR("\""));

        CFStringRef string = plist;
        CFIndex length = CFStringGetLength(string);
        UniChar *buffer = malloc(sizeof(*buffer) * length);
        CFStringGetCharacters(string, CFRangeMake(0, length), buffer);
        for(CFIndex i = 0; i < length; i++) {
            UniChar ch = buffer[i];
            if(ch == '\\')
                CFStringAppend(output, CFSTR("\\\\"));
            else if(ch == '"')
                CFStringAppend(output, CFSTR("\\\""));
            else if(ch == '\0')
                CFStringAppend(output, CFSTR("\\0"));
            else
                CFStringAppendCharacters(output, &ch, 1);
        }
        free(buffer);
        
        CFStringAppend(output, CFSTR("\""));
        return;
    }
    if(type == CFNumberGetTypeID()) {
        CFNumberRef number = plist;
        long long ll;
        double d;
        if(CFNumberGetValue(number, kCFNumberLongLongType, &ll)) {
            CFStringAppendFormat(output, NULL, CFSTR("%lld"), ll);
            return;
        } else if(CFNumberGetValue(number, kCFNumberDoubleType, &d)) {
            CFStringAppendFormat(output, NULL, CFSTR("%f"), d);
            return;
        }
    }
    CFStringRef indent2 = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@   "), indent);
    if(type == CFArrayGetTypeID()) {
        CFStringAppend(output, CFSTR("(\n"));
        CFArrayRef array = plist;
        CFIndex count = CFArrayGetCount(array);
        for(CFIndex i = 0; i < count; i++) {
            CFStringAppend(output, indent2);
            serialize_openstep_recurse(CFArrayGetValueAtIndex(array, i), indent2, output);
            CFStringAppend(output, CFSTR(",\n"));
        }
        CFStringAppend(output, indent);
        CFStringAppend(output, CFSTR(")"));
        CFRelease(indent2);
        return;
    }
    if(type == CFDictionaryGetTypeID()) {
        CFStringAppend(output, CFSTR("{\n"));

        CFDictionaryRef dict = plist;
        CFIndex count = CFDictionaryGetCount(dict);
        const void **keys = malloc(sizeof(*keys) * count);
        const void **values = malloc(sizeof(*keys) * count);
        CFDictionaryGetKeysAndValues(dict, keys, values);
        for(CFIndex i = 0; i < count; i++) {
            CFStringAppend(output, indent2);
            serialize_openstep_recurse(keys[i], indent2, output);
            CFStringAppend(output, CFSTR(" = "));
            serialize_openstep_recurse(values[i], indent2, output);
            CFStringAppend(output, CFSTR(";\n"));
        }
        free(keys);
        free(values);
        CFStringAppend(output, indent);
        CFStringAppend(output, CFSTR("}"));
        CFRelease(indent2);
        return;
    }
    // some other object
    CFRelease(indent2);
    CFStringAppend(output, CFCopyDescription(plist));
}

static CFDataRef serialize_openstep(CFPropertyListRef plist) {
    CFMutableStringRef string = CFStringCreateMutable(NULL, 0);
    serialize_openstep_recurse(plist, CFSTR(""), string);
    CFStringAppend(string, CFSTR("\n"));
    CFDataRef result = CFStringCreateExternalRepresentation(NULL, string, kCFStringEncodingUTF8, '?');
    CFRelease(string);
    return result;
}


static void write_it(CFPropertyListRef plist, const char *urlname, CFURLRef url, CFPropertyListFormat format) {
    CFErrorRef cferror;
    CFDataRef data;

    if(format == kCFPropertyListOpenStepFormat) {
        data = serialize_openstep(plist);
    } else {
        data = CFPropertyListCreateData(NULL, plist, format, 0, &cferror);
        if(!data) {
            fprintf(stderr, "Couldn't create data: %s\n", cferror_to_string(cferror));
            exit(1);
        }
    }
   
    if(url) {
        SInt32 error;
        if(!CFURLWriteDataAndPropertiesToResource(url, data, NULL, &error)) {
            fprintf(stderr, "Couldn't write %s: %s\n", urlname, urlerror_to_str(error));
            exit(1);
        }
        CFRelease(url);
    } else {
        write(1, CFDataGetBytePtr(data), CFDataGetLength(data));
    }
    CFRelease(data);
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

    bool wrote = false;

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

            if(!dots((void *) plist, key, MODE_SET, (void *) valuepl)) exit(1);

            CFRelease(data);
            CFRelease(valuepl);

            argptr += 3;
        } else if(!strcmp(*argptr, "-r")) {
            if(!argptr[1]) usage();
            char *key = argptr[1];

            if(!dots((void *) plist, key, MODE_REMOVE, NULL)) exit(1);

            argptr += 2;
        } else if(!strcmp(*argptr, "-w") || !strcmp(*argptr, "-x") || !strcmp(*argptr, "-o")) {
            if(!argptr[1]) usage();
            char mode = (*argptr)[1];
            if(mode == 'x')
                format = kCFPropertyListXMLFormat_v1_0;
            else if(mode == 'o')
                format = kCFPropertyListOpenStepFormat;
            CFURLRef url2 = strcmp(argptr[1], "-") ? filename_to_url(argptr[1]) : NULL;
            write_it(plist, argptr[1], url2, format);

            wrote = true;
            argptr += 2;
        } else {
            void *out = NULL;
            if(!dots((void *) plist, *argptr, MODE_GET, &out)) exit(1);
            show(stdout, out);

            argptr++;
        }
    }

    if(!wrote) {
        write_it(plist, "-", NULL, kCFPropertyListOpenStepFormat);
    }

    return 0;
}
