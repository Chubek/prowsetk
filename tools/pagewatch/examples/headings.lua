local watch = require('lpgwatch')
function main(args)
    watch.watch { session=session, query=args.query }
    return 0
end
